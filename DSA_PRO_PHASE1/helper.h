#pragma once

#include <iostream>
#include <string>
#include <vector>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <algorithm>

using namespace std;

const int32_t MAX_VARS_PER_FRAME = 16;
const int32_t MAX_STACK_DEPTH = 64;

const int32_t IO_BUFFER_SIZE = 64 * 1024;
const int32_t SOCKET_TIMEOUT_SEC = 5;
const int32_t MAX_FUNCS = 128;
const int32_t MAX_TOKENS = MAX_VARS_PER_FRAME + 2;
const int32_t MAX_PATCHES = MAX_FUNCS * 4;

const uint64_t MAX_SOURCE_BYTES = 15ULL * 1024 * 1024;
struct FuncEntry
{
    string funcName;
    int64_t byteOffsetInResolveBin; // where this function's FUNC header record sits
};
struct PendingPatch
{
    int64_t byteOffsetOfOffsetField; // where in resolve.bin to seek back and overwrite
    string targetFuncName;
};
struct Variable
{
    string name;
    int32_t value;
};
struct Frame
{
    string func_name;
    int32_t argc;
    Variable argv[MAX_VARS_PER_FRAME];
    int64_t returnLine;
    Variable locals[MAX_VARS_PER_FRAME];
    int32_t localCount;
};
struct Snapshot
{
    Frame callStack[MAX_STACK_DEPTH];
    int32_t stackDepth;
};
struct TTDBHeader
{
    char magic[4];
    int32_t version;
    int32_t stepCount;
    int64_t indexOffset;
};
class CRC32 {
private:
    uint32_t crc;
public:
    CRC32() {
        reset();
    }

    void reset() {
        crc = 0xFFFFFFFF;
    }
    void update(const char* data, size_t length) {
        for (size_t i = 0; i < length; i++) {
            crc = crc ^ static_cast<unsigned char>(data[i]);

            for (int bit = 0; bit < 8; bit++) {
                if (crc & 1) {
                    crc = (crc >> 1) ^ 0xEDB88320;
                }
                else {
                    crc >>= 1;
                }
            }
        }
    }

    uint32_t value() const {
        return crc ^ 0xFFFFFFFF;
    }
};

class TrailerChecker {
private:

    CRC32 checksum;

    vector<char> trailer;

public:
    TrailerChecker() {
        trailer.reserve(4);
    }
    void feed(const char* data, size_t length, vector<char>& actualBytes) {
        actualBytes.clear();

        vector<char> current_byte;

        current_byte.reserve(trailer.size() + length);

        current_byte.insert(current_byte.end(), trailer.begin(), trailer.end());

        current_byte.insert(current_byte.end(), data, data + length);

        if (current_byte.size() <= 4) {
            trailer = current_byte;
            return;
        }

        size_t actualBytesCt = current_byte.size() - 4;

        actualBytes.insert(actualBytes.end(), current_byte.begin(), current_byte.begin() + actualBytesCt);

        checksum.update(actualBytes.data(), actualBytes.size());

        trailer.assign(current_byte.end() - 4, current_byte.end());
    }

    bool verify() const {
        if (trailer.size() != 4) {
            return false;
        }

        uint32_t receivedChecksum =(static_cast<uint32_t>(static_cast<unsigned char>(trailer[0])) << 24) | (static_cast<uint32_t>(static_cast<unsigned char>(trailer[1])) << 16) | (static_cast<uint32_t>(static_cast<unsigned char>(trailer[2])) << 8) | static_cast<uint32_t>(static_cast<unsigned char>(trailer[3]));

        if (receivedChecksum == checksum.value()) {
            return true;
        }
        else {
            return false;
        }
    }
    const vector<char>& getTrailer() const{
        return trailer;
    }
    
};