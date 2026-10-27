#include "bgiplayer/filter_color.hpp"
#include <algorithm>
#include <cstddef>
namespace bgiplayer {
bool filter_luminance_color(std::span<uint8_t> destination,
    uint32_t destination_stride, std::span<const uint8_t> source,
    uint32_t source_stride, uint32_t width, uint32_t height,
    uint32_t format, uint32_t color, uint32_t weight) {
    if ((format != 1 && format != 2) || weight > 256 ||
        uint64_t(width)*4 > destination_stride || uint64_t(width)*4 > source_stride ||
        uint64_t(destination_stride)*height > destination.size() ||
        uint64_t(source_stride)*height > source.size()) return false;
    for (uint32_t y=0;y<height;++y) {
        const auto* src=source.data()+size_t(y)*source_stride;
        auto* dst=destination.data()+size_t(y)*destination_stride;
        for (uint32_t x=0;x<width;x+=2) {
            const uint32_t count=std::min(2U,width-x);
            // The SSE pair shortcut checks both alphas together. A single
            // transparent pixel paired with an opaque one still gets RGB.
            const bool zero=format==2 && src[x*4U+3]==0 &&
                (count==1 || src[x*4U+7]==0);
            for(uint32_t lane=0;lane<count;++lane) {
                const auto* p=src+(x+lane)*4U;
                auto* q=dst+(x+lane)*4U;
                if(zero) { std::fill_n(q,4,uint8_t{0}); continue; }
                const uint32_t luminance=p[0]*29U+p[1]*150U+p[2]*77U;
                for(uint32_t channel=0;channel<3;++channel) {
                    const int32_t tinted=int32_t((luminance*((color>>(channel*8U))&255U))>>16U);
                    const int32_t value=int32_t(p[channel])+
                        (((tinted-int32_t(p[channel]))*int32_t(weight>>1U))>>7);
                    q[channel]=uint8_t(std::clamp(value,0,255));
                }
                q[3]=p[3];
            }
        }
    }
    return true;
}
bool filter_black_alpha(std::span<uint8_t> destination, uint32_t destination_stride,
    std::span<const uint8_t> source, uint32_t source_stride,
    uint32_t width, uint32_t height, uint32_t weight) {
    if (weight > 256 || uint64_t(width)*4 > destination_stride ||
        uint64_t(width)*4 > source_stride ||
        uint64_t(destination_stride)*height > destination.size() ||
        uint64_t(source_stride)*height > source.size()) return false;
    for (uint32_t y=0;y<height;++y) for (uint32_t x=0;x<width;++x) {
        const auto* p=source.data()+size_t(y)*source_stride+x*4U;
        auto* q=destination.data()+size_t(y)*destination_stride+x*4U;
        // 0040EA60 strength zero dispatches to the same-format raw copy.
        if (weight==0) { std::copy_n(p,4,q); continue; }
        const uint32_t alpha=p[3];
        if (alpha==0) continue; // Transparent source leaves every destination byte.
        const uint32_t retained=(256U-alpha)*q[3];
        const uint32_t denominator=alpha*256U+retained;
        const uint32_t foreground=(alpha*(256U-weight)<<16U)/denominator;
        const uint32_t background=(retained<<16U)/denominator;
        for (uint32_t channel=0;channel<3;++channel)
            q[channel]=uint8_t((p[channel]*foreground+q[channel]*background)>>16U);
        q[3]=uint8_t(denominator>>8U);
    }
    return true;
}
bool filter_xor_color(std::span<uint8_t> destination, uint32_t destination_stride,
    std::span<const uint8_t> source, uint32_t source_stride,
    uint32_t width, uint32_t height, uint32_t format, uint32_t color, uint32_t weight) {
    if ((format!=1 && format!=2) || weight>256 ||
        uint64_t(width)*4>destination_stride || uint64_t(width)*4>source_stride ||
        uint64_t(destination_stride)*height>destination.size() ||
        uint64_t(source_stride)*height>source.size()) return false;
    for(uint32_t y=0;y<height;++y) {
        const auto* src=source.data()+size_t(y)*source_stride;
        auto* dst=destination.data()+size_t(y)*destination_stride;
        for(uint32_t x=0;x<width;x+=2) {
            const uint32_t count=std::min(2U,width-x);
            const bool zero=format==2 && src[x*4U+3]==0 && (count==1 || src[x*4U+7]==0);
            for(uint32_t lane=0;lane<count;++lane) {
                const auto* p=src+(x+lane)*4U;auto* q=dst+(x+lane)*4U;
                if(zero){std::fill_n(q,4,uint8_t{0});continue;}
                for(uint32_t channel=0;channel<3;++channel) {
                    const int32_t input=p[channel],target=input^int32_t((color>>(channel*8U))&255U);
                    q[channel]=uint8_t(std::clamp(input+(((target-input)*int32_t(weight>>1U))>>7),0,255));
                }
                q[3]=p[3];
            }
        }
    }
    return true;
}
bool filter_addsub_color(std::span<uint8_t> destination, uint32_t destination_stride,
    std::span<const uint8_t> source, uint32_t source_stride,
    uint32_t width, uint32_t height, uint32_t format, uint32_t color, uint32_t weight) {
    if ((format!=1 && format!=2) || weight>256 || uint64_t(width)*4>destination_stride ||
        uint64_t(width)*4>source_stride || uint64_t(destination_stride)*height>destination.size() ||
        uint64_t(source_stride)*height>source.size()) return false;
    const bool subtract=color==0;
    const uint32_t tint=subtract?0xffffffU:color;
    for(uint32_t y=0;y<height;++y)for(uint32_t x=0;x<width;++x){
        const auto* p=source.data()+size_t(y)*source_stride+x*4U;
        auto* q=destination.data()+size_t(y)*destination_stride+x*4U;
        for(uint32_t c=0;c<3;++c){
            const int32_t delta=int32_t((((tint>>(c*8U))&255U)*weight)>>8U);
            q[c]=uint8_t(std::clamp(int32_t(p[c])+(subtract?-delta:delta),0,255));
        }
        q[3]=p[3];
    }
    return true;
}
} // namespace bgiplayer
