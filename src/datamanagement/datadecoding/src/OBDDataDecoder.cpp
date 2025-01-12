/**
 * Copyright 2020 Amazon.com, Inc. and its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: LicenseRef-.amazon.com.-AmznSL-1.0
 * Licensed under the Amazon Software License (the "License").
 * You may not use this file except in compliance with the License.
 * A copy of the License is located at
 * http://aws.amazon.com/asl/
 * or in the "license" file accompanying this file. This file is distributed
 * on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 * express or implied. See the License for the specific language governing
 * permissions and limitations under the License.
 */

// Includes
#include "OBDDataDecoder.h"
#include "EnumUtility.h"
#include <algorithm>
#include <ios>
#include <sstream>
constexpr int POSITIVE_ECU_RESPONSE_BASE = 0x40;
#define IS_BIT_SET( var, pos ) ( ( var ) & ( 1 << ( pos ) ) )

namespace Aws
{
namespace IoTFleetWise
{
namespace DataManagement
{

OBDDataDecoder::OBDDataDecoder()
{
    mTimer.reset();
}

bool
OBDDataDecoder::decodeSupportedPIDs( const SID &sid,
                                     const std::vector<uint8_t> &inputData,
                                     SupportedPIDs &supportedPIDs )
{
    // First look at whether we received a positive response
    // The positive response can be identified by 0x40 + SID.
    // If the input size is less than 6 ( Response byte + Requested PID + 4 data bytes )
    // this is also not a valid input
    if ( inputData.size() < 6 || POSITIVE_ECU_RESPONSE_BASE + toUType( sid ) != inputData[0] )
    {
        mLogger.warn( "OBDDataDecoder::decodeSupportedPIDs", "Invalid Supported PID Input" );
        return false;
    }
    // Make sure we put only the ones we support by our software in the result.
    // The reason for that is if we request something we don't support yet
    // we cannot parse the response correctly i.e. number of bytes per PID is NOT fixed
    // The structure of a positive response should be according to :
    // Section 8.1.2.2 Request Current Powertrain Diagnostic Data Response Message Definition (report supported PIDs)
    // from the J1979 spec
    // 0x41(Positive response), 0x00( requested PID range), 4 Bytes, 0x20( requested PID range), 4 Bytes. etc
    uint8_t basePIDCount = 0;
    for ( size_t i = 1; i < inputData.size(); ++i )
    {
        // First extract the PID Range, its position is always ByteIndex mod 5
        if ( ( i % 5 ) == 1 )
        {
            basePIDCount++;
            // Skip this byte
            continue;
        }
        for ( size_t j = 0; j < BYTE_SIZE; ++j )
        {
            if ( IS_BIT_SET( inputData[i], j ) )
            {
                // E.g. basePID = 0x20, and j = 2, put SID1_PID_34 in the result
                size_t index = ( i - basePIDCount ) * BYTE_SIZE - j;
                PID decodedID = getPID( sid, index );
                // The response includes the PID range requested.
                // To remain consistent with the spec, we don't want to mix Supported PID IDs with
                // PIDs. We validate that the PID received is supported by the software, but also
                // exclude the Supported PID IDs from the output list
                if ( decodedID != INVALID_PID &&
                     std::find( supportedPIDRange.begin(), supportedPIDRange.end(), decodedID ) ==
                         supportedPIDRange.end() )
                {
                    supportedPIDs.emplace_back( decodedID );
                }
            }
        }
    }
    // Sort the result for easy lookup
    std::sort( supportedPIDs.begin(), supportedPIDs.end() );

    return !supportedPIDs.empty();
}

void
OBDDataDecoder::setDecoderDictionary( ConstOBDDecoderDictionaryConstPtr &dictionary )
{
    // OBDDataDecoder is running in one thread, hence we don't need mutext to prevent race condition
    mDecoderDictionaryConstPtr = dictionary;
}

bool
OBDDataDecoder::decodeEmissionPIDs( const SID &sid,
                                    const std::vector<PID> &pids,
                                    const std::vector<uint8_t> &inputData,
                                    EmissionInfo &info )
{
    // First look at whether we received a positive response
    // The positive response can be identified by 0x40 + SID.
    // If the input size is less than 3 ( Positive Response + Response byte + Requested PID )

    // this is also not a valid input as we expect at least one by response.
    if ( inputData.size() < 3 || POSITIVE_ECU_RESPONSE_BASE + toUType( sid ) != inputData[0] )
    {
        mLogger.warn( "OBDDataDecoder::decodeEmissionPIDs", "Invalid response to PID request" );
        return false;
    }
    if ( mDecoderDictionaryConstPtr == nullptr )
    {
        mLogger.warn( "OBDDataDecoder::decodeEmissionPIDs", "Invalid Decoder Dictionary!" );
        return false;
    }
    // Validate 1) The PIDs in response match with expected PID; 2) Total length of PID response matches with Decoder
    // Manifest. If not matched, the program will discard this response and not attempt to decode.
    if ( isPIDResponseValid( pids, inputData ) == false )
    {
        mLogger.warn( "OBDDataDecoder::decodeEmissionPIDs", "Invalid PIDs response" );
        return false;
    }
    // Setup the Info
    info.mSID = sid;
    // Start from byte number 2 which is the PID requested
    size_t byteCounter = 1;
    while ( byteCounter < inputData.size() )
    {
        auto pid = inputData[byteCounter++];
        // first check whether the decoder dictionary contains this PID
        if ( mDecoderDictionaryConstPtr->find( pid ) != mDecoderDictionaryConstPtr->end() )
        {
            // The expected number of bytes returned from PID
            auto expectedResponseLength = mDecoderDictionaryConstPtr->at( pid ).mSizeInBytes;
            auto formulas = mDecoderDictionaryConstPtr->at( pid ).mSignals;
            // first check whether we have received enough bytes for this PID
            if ( byteCounter + expectedResponseLength <= inputData.size() )
            {
                // check how many signals do we need to collect from this PID.
                // This is defined in cloud decoder manifest.
                // Each signal has its associated formula
                for ( auto formula : formulas )
                {
                    // Before using formula, check it against rule
                    if ( isFormulaValid( pid, formula ) )
                    {
                        // In J1979 spec, longest value has 4-byte.
                        // Allocate 64-bit here in case signal value increased in the future
                        uint64_t rawData = 0;
                        size_t byteIdx = byteCounter + ( formula.mFirstBitPosition / BYTE_SIZE );
                        // If the signal length is less than 8-bit, we need to perform bit field operation
                        if ( formula.mSizeInBits < BYTE_SIZE )
                        {
                            // bit manipulation performed here: shift first, then apply mask
                            // e.g. If signal are bit 4 ~ 7 in Byte A.
                            // we firstly right shift by 4, then apply bit mask 0b1111
                            rawData = inputData[byteIdx];
                            rawData >>= formula.mFirstBitPosition % BYTE_SIZE;
                            rawData &= ( 0xFF >> ( BYTE_SIZE - formula.mSizeInBits ) );
                        }
                        else
                        {
                            // This signal contain greater or equal than one byte, concatenate raw bytes
                            auto numOfBytes = formula.mSizeInBits / BYTE_SIZE;
                            // This signal contains multiple bytes, concatenate the bytes
                            while ( numOfBytes != 0 )
                            {
                                --numOfBytes;
                                rawData = ( rawData << BYTE_SIZE ) | inputData[byteIdx++];
                            }
                        }
                        // apply scaling and offset to the raw data.
                        info.mPIDsToValues.emplace( formula.mSignalID,
                                                    static_cast<SignalValue>( rawData ) * formula.mFactor +
                                                        formula.mOffset );
                    }
                }
            }
            // Done with this PID, move on to next PID by increment byteCounter by response length of current PID
            byteCounter += expectedResponseLength;
        }
        else
        {
            mLogger.trace( "OBDDataDecoder::decodeEmissionPIDs",
                           "PID " + std::to_string( pid ) + " missing in decoder dictionary" );
            // Cannot decode this byte as it doesn't exist in both decoder dictionary
            // Cannot proceed with the rest of response because the payload might already be misaligned.
            // Note because we already checked the response validity with isPIDResponseValid(), the program should
            // not come here logically. But it might still happen in rare case such as bit flipping.
            break;
        }
    }
    return !info.mPIDsToValues.empty();
}

bool
OBDDataDecoder::decodeDTCs( const SID &sid, const std::vector<uint8_t> &inputData, DTCInfo &info )
{
    // First look at whether we received a positive response
    // The positive response can be identified by 0x40 + SID.
    // If an ECU has no DTCs, it should respond with 2 Bytes ( 1 for Positive response + 1 number of DTCs( 0) )
    if ( inputData.size() < 2 || POSITIVE_ECU_RESPONSE_BASE + toUType( sid ) != inputData[0] )
    {
        return false;
    }
    info.mSID = sid;
    // First Byte is the DTC count
    size_t dtcCount = inputData[1];
    // The next bytes are expected to be the actual DTCs.
    if ( dtcCount == 0 )
    {
        // No DTC reported, all good
        return true;
    }
    else
    {
        // Expect the size of the ECU response to be 2 + the 2 bytes for each DTC
        if ( ( dtcCount * 2 ) + 2 != inputData.size() )
        {
            // Corrupt frame
            return false;
        }
        // Process the DTCs in a chunk of 2 bytes
        std::string dtcString;
        for ( size_t byteIndex = 2; byteIndex < inputData.size() - 1; byteIndex += 2 )
        {

            if ( extractDTCString( inputData[byteIndex], inputData[byteIndex + 1], dtcString ) )
            {
                info.mDTCCodes.emplace_back( dtcString );
            }
        }
    }

    return !info.mDTCCodes.empty();
}

bool
OBDDataDecoder::extractDTCString( const uint8_t &firstByte, const uint8_t &secondByte, std::string &dtcString )
{
    dtcString.clear();
    std::stringstream stream;
    // Decode the DTC Domain according to J1979 8.3.1
    // Extract the first 2 bits of the first Byte
    switch ( firstByte >> 6 )
    {
    case toUType( DTCDomains::POWERTRAIN ): // Powertrain
        stream << 'P';
        break;
    case toUType( DTCDomains::CHASSIS ): // Powertrain
        stream << 'C';
        break;
    case toUType( DTCDomains::BODY ): // Powertrain
        stream << 'B';
        break;
    case toUType( DTCDomains::NETWORK ): // Powertrain
        stream << 'U';
        break;
    default:
        break;
    }

    // Extract the first digit of the DTC ( second 2 bits from first byte)
    stream << std::hex << ( ( firstByte & 0x30 ) >> 4 );
    // Next digit is the last 4 bits of the first byte
    stream << std::hex << ( firstByte & 0x0F );
    // Next digit is the first 4 bits of the second byte
    stream << std::hex << ( secondByte >> 4 );
    // Next digit is the last 4 bits of the second byte
    stream << std::hex << ( secondByte & 0x0F );
    dtcString = stream.str();
    // Apply upper case before returning
    std::transform( dtcString.begin(), dtcString.end(), dtcString.begin(), ::toupper );
    return !dtcString.empty();
}

bool
OBDDataDecoder::decodeVIN( const std::vector<uint8_t> &inputData, std::string &vin )
{
    // First look at whether we received a positive response
    // The positive response can be identified by 0x40 + SID.
    // The response is usually 1 byte of the positive response, 1 byte for the InfoType(PID), 1 byte for the number of
    // data item.
    if ( inputData.size() < 3 ||
         POSITIVE_ECU_RESPONSE_BASE + toUType( vehicleIdentificationNumberRequest.mSID ) != inputData[0] ||
         vehicleIdentificationNumberRequest.mPID != inputData[1] )
    {
        return false;
    }
    // Assign the rest of the vector to the output string
    vin.assign( inputData.begin() + 3, inputData.end() );
    return !vin.empty();
}

bool
OBDDataDecoder::isPIDResponseValid( const std::vector<PID> &pids, const std::vector<uint8_t> &ecuResponse )
{
    // This index is used to iterate through the ECU PID response length
    // As the first byte in response is the Service Mode, we will start from the second byte.
    size_t responseByteIndex = 1;
    for ( auto pid : pids )
    {
        // if the response length is shorter than expected or the PID in ECU response mismatches with
        // the requested PID, it's an invalid ECU response
        if ( responseByteIndex >= ecuResponse.size() || ecuResponse[responseByteIndex] != pid )
        {
            mLogger.warn( "OBDDataDecoder::isPIDResponseValid",
                          "Cannot find PID " + std::to_string( pid ) + " in ECU response" );
            return false;
        }
        if ( mDecoderDictionaryConstPtr->find( pid ) != mDecoderDictionaryConstPtr->end() )
        {
            // Move Index into the next PID
            responseByteIndex += ( mDecoderDictionaryConstPtr->at( pid ).mSizeInBytes + 1 );
        }
        else
        {
            mLogger.warn( "OBDDataDecoder::isPIDResponseValid",
                          "PID " + std::to_string( pid ) + " not found in decoder dictionary" );
            return false;
        }
    }
    if ( responseByteIndex != ecuResponse.size() )
    {
        mLogger.warn( "OBDDataDecoder::isPIDResponseValid",
                      "Expect response length: " + std::to_string( responseByteIndex ) +
                          " Actual response length: " + std::to_string( ecuResponse.size() ) );
    }
    return responseByteIndex == ecuResponse.size();
}

bool
OBDDataDecoder::isFormulaValid( PID pid, CANSignalFormat formula )
{
    bool isValid = false;
    // Here's the rules we apply to check whether PID formula is valid
    // 1. First Bit Position has to be less than last bit position of PID response length
    // 2. Last Bit Position (first bit + sizeInBits) has to be less than or equal to last bit position of PID response
    // length
    // 3. If mSizeInBits are greater or equal than 8, both mSizeInBits and first bit position has to be multiple of 8
    if ( mDecoderDictionaryConstPtr->find( pid ) != mDecoderDictionaryConstPtr->end() &&
         formula.mFirstBitPosition < mDecoderDictionaryConstPtr->at( pid ).mSizeInBytes * BYTE_SIZE &&
         ( formula.mSizeInBits + formula.mFirstBitPosition <=
           mDecoderDictionaryConstPtr->at( pid ).mSizeInBytes * BYTE_SIZE ) &&
         ( formula.mSizeInBits < 8 ||
           ( ( formula.mSizeInBits & 0x7 ) == 0 && ( formula.mFirstBitPosition & 0x7 ) == 0 ) ) )
    {
        isValid = true;
    }
    return isValid;
}

} // namespace DataManagement
} // namespace IoTFleetWise
} // namespace Aws
