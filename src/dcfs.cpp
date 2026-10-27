#include "bgiplayer/dcfs.hpp"
#include "bgiplayer/common.hpp"
#include <algorithm>
#include <limits>
#include <string_view>
namespace bgiplayer {
std::vector<uint8_t> encode_dcfs_payload(std::span<const uint8_t> input,
    uint32_t row_bytes, uint32_t rows) {
    const uint64_t size=uint64_t(row_bytes)*rows;
    if (rows==0 || row_bytes==0 || size>UINT32_MAX || size>input.size())
        throw Error("invalid DCFS source dimensions or span");
    std::vector<uint8_t> result(0x18,0);
    constexpr std::string_view magic="DCFS FORMAT 1.00";
    std::copy(magic.begin(),magic.end(),result.begin());
    for (unsigned byte=0;byte<4;++byte) {
        result[0x10+byte]=static_cast<uint8_t>(row_bytes>>(byte*8));
        result[0x14+byte]=static_cast<uint8_t>(rows>>(byte*8));
    }
    result.insert(result.end(),input.begin(),input.begin()+row_bytes);
    const auto append_length=[&](uint32_t length) {
        // Original14008D610 unsigned7-bit groups, including a one-byte zero
        // when a row starts with a differing byte.
        do {
            const uint8_t byte=static_cast<uint8_t>(length&0x7fU);
            length>>=7U;
            result.push_back(static_cast<uint8_t>(byte|(length?0x80U:0U)));
        } while(length);
    };
    for (uint32_t row=1;row<rows;++row) {
        const size_t previous=size_t(row-1U)*row_bytes;
        const size_t current=size_t(row)*row_bytes;
        uint32_t offset=0;
        bool equal=true;
        while(offset<row_bytes) {
            uint32_t length=0;
            while(length<row_bytes-offset &&
                  ((input[previous+offset+length]==input[current+offset+length])==equal))
                ++length;
            append_length(length);
            if(!equal) {
                const auto literals=input.subspan(current+offset,length);
                result.insert(result.end(),literals.begin(),literals.end());
            }
            offset+=length;
            equal=!equal;
        }
    }
    if(result.size()>UINT32_MAX)throw Error("DCFS encoded payload exceeds container limit");
    return result;
}
}
