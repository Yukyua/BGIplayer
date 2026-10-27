#include "bgiplayer/filter_mask.hpp"
#include <algorithm>
#include <bit>
#include <cstddef>
namespace bgiplayer {
bool filter_bitmap_mask(std::span<uint8_t> destination,uint32_t stride,
    std::span<const uint8_t> mask,uint32_t mask_stride,uint32_t width,uint32_t height,
    uint32_t color,uint32_t shift,uint32_t weight) {
    if(weight>256 || uint64_t(width)*4>stride || width>mask_stride ||
       (height ? uint64_t(height-1)*stride+uint64_t(width)*4 : 0)>destination.size() ||
       (height ? uint64_t(height-1)*mask_stride+width : 0)>mask.size())return false;
    shift&=31U;
    const uint32_t bias=256U-(weight<<shift)-weight;
    for(uint32_t y=0;y<height;++y)for(uint32_t x=0;x<width;++x){
        auto* p=destination.data()+size_t(y)*stride+x*4U;
        const int32_t parameter=std::bit_cast<int32_t>((uint32_t(mask[size_t(y)*mask_stride+x])<<shift)+bias);
        if(parameter>=256)continue;
        for(uint32_t c=0;c<3;++c){
            const int32_t tint=int32_t((color>>(c*8))&255U);
            p[c]=uint8_t(parameter<=0?tint:std::clamp(tint+((int32_t(p[c])-tint)*(parameter>>1)>>7),0,255));
        }
        p[3]=0;
    }
    return true;
}
} // namespace bgiplayer
