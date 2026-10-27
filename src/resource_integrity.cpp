#include "bgiplayer/resource_integrity.hpp"
#include <algorithm>
namespace bgiplayer {
std::array<uint8_t,8> resource_checksum(std::span<const uint8_t> data) {
    std::array<uint8_t,8> state{};
    uint32_t sum=0;
    for(const auto byte:data){
        sum=sum*0xE9U+byte;
        const auto low=static_cast<uint8_t>(sum);
        state[4]=static_cast<uint8_t>(state[4]+low);
        state[5]^=low;
        state[6]=static_cast<uint8_t>(state[6]+byte);
        state[7]^=byte;
    }
    for(unsigned i=0;i<4;++i)state[i]=static_cast<uint8_t>(sum>>(i*8U));
    return state;
}
uint32_t check_resource_integrity(const ResourceIntegrityData& data) {
    if(data.read_status!=0)return data.read_status;
    if(std::all_of(data.expected.begin(),data.expected.end(),[](auto x){return x==0;}))return 10;
    return resource_checksum(data.payload)==data.expected?0U:11U;
}
}
