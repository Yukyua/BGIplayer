#include "bgiplayer/filter_rgb555.hpp"
#include <cstddef>
namespace bgiplayer {
bool filter_rgb555_color(std::span<uint8_t> destination,uint32_t stride,
    std::span<const uint8_t> source,uint32_t source_stride,uint32_t width,uint32_t height,
    uint32_t color,uint32_t weight){
    if(weight>256 || uint64_t(width)*2>stride || uint64_t(width)*2>source_stride ||
       (height?uint64_t(height-1)*stride+uint64_t(width)*2:0)>destination.size() ||
       (height?uint64_t(height-1)*source_stride+uint64_t(width)*2:0)>source.size())return false;
    const uint32_t red=(color>>9U)&0x7c00U,green=(color>>6U)&0x3e0U,blue=(color>>3U)&31U;
    for(uint32_t y=0;y<height;++y)for(uint32_t index=0;index<width;++index){
        // Both original color kernels walk right to left. The distinct
        // color0/strength0 raw-copy entry is used in place by VM callbacks.
        const uint32_t x=(color!=0 || weight!=0)?width-1U-index:index;
        const auto*p=source.data()+size_t(y)*source_stride+x*2U;auto*q=destination.data()+size_t(y)*stride+x*2U;
        const uint32_t v=p[0]|(uint32_t(p[1])<<8U);uint32_t result=v;
        if(color!=0 || weight!=0)result=((((v&0x7c00U)*(256U-weight)+red*weight)>>8U)&0x7c00U)|
            ((((v&0x3e0U)*(256U-weight)+green*weight)>>8U)&0x3e0U)|
            (((v&31U)*(256U-weight)+blue*weight)>>8U);
        q[0]=uint8_t(result);q[1]=uint8_t(result>>8U);
    }
    return true;
}
} // namespace bgiplayer
