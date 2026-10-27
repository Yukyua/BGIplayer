#include "bgiplayer/sdc_encode.hpp"
#include "bgiplayer/common.hpp"
#include <array>
#include <deque>
#include <limits>
namespace bgiplayer {
std::vector<uint8_t> compress_original_sdc_payload(std::span<const uint8_t> input) {
    // 1400F6010..60DF searches newest first and keeps the first equal-length
    // match. Distance1 is deliberately excluded; overlapping distance2 is valid.
    std::array<std::deque<std::size_t>,256> chains;
    std::vector<uint8_t> out;
    std::size_t position=0,literal_start=0,literals=0;
    const auto append=[&](std::size_t p){
        // Removal follows matching, so distance2049 is still searchable.
        if(p>=2049)chains[input[p-2049]].pop_back();
        chains[input[p]].push_front(p);
    };
    const auto flush=[&](){
        if(literals==0)return;
        out.push_back(static_cast<uint8_t>(literals-1));
        out.insert(out.end(),input.begin()+static_cast<std::ptrdiff_t>(literal_start),
                   input.begin()+static_cast<std::ptrdiff_t>(literal_start+literals));
        literals=0;
    };
    while(position<input.size()){
        std::size_t best=0,distance=0;
        const std::size_t maximum=std::min<std::size_t>(17,input.size()-position);
        for(const auto previous:chains[input[position]]){
            const auto delta=position-previous;
            if(delta==1)continue;
            std::size_t length=1;
            while(length<maximum && input[position+length]==input[position+length-delta])++length;
            if(length>1 && length>best){best=length;distance=delta;}
            if(length==maximum)break;
        }
        if(best==0){
            append(position++);
            ++literals;
        }
        if(literals>=128 || best>1)flush();
        if(best>1){
            for(std::size_t i=0;i<best;++i)append(position++);
            const auto command=static_cast<uint16_t>(0x8000U+((best-2U)<<11U)+(distance-2U));
            out.push_back(static_cast<uint8_t>(command>>8U));
            out.push_back(static_cast<uint8_t>(command));
        }
        if(literals==0)literal_start=position;
    }
    flush();
    return out;
}
std::vector<uint8_t> encode_original_sdc(std::span<const uint8_t> input,uint16_t seed){
    if(input.size()>std::numeric_limits<uint32_t>::max())throw Error("SDC input too large");
    auto commands=compress_original_sdc_payload(input);
    if(commands.size()>std::numeric_limits<uint32_t>::max())throw Error("SDC command stream too large");
    std::vector<uint8_t> out(0x20,0);
    constexpr char magic[]="SDC FORMAT 1.00";
    std::copy_n(magic,16,out.begin());
    const auto store=[&](std::size_t at,uint32_t value){for(unsigned b=0;b<4;++b)out[at+b]=static_cast<uint8_t>(value>>(b*8U));};
    store(0x10,seed);store(0x14,static_cast<uint32_t>(commands.size()));store(0x18,static_cast<uint32_t>(input.size()));
    uint32_t state=seed;uint16_t sum=0,xor_value=0;
    // 1400F5A20 /1400F5E70, identical split-word PRNG to original decoder.
    for(auto& byte:commands){
        const uint32_t low=(state&0xFFFFU)*0x4E35U;
        const uint32_t mixed=(state>>16U)*0x4E35U+state*0x15AU+(low>>16U);
        state=(low&0xFFFFU)+1U+(mixed<<16U);
        byte=static_cast<uint8_t>(byte+static_cast<uint8_t>(mixed));
        sum=static_cast<uint16_t>(sum+byte);xor_value=static_cast<uint16_t>(xor_value^byte);
    }
    out[0x1C]=static_cast<uint8_t>(sum);out[0x1D]=static_cast<uint8_t>(sum>>8U);
    out[0x1E]=static_cast<uint8_t>(xor_value);out[0x1F]=static_cast<uint8_t>(xor_value>>8U);
    out.insert(out.end(),commands.begin(),commands.end());return out;
}
}
