#include "bgiplayer/background_mask.hpp"
#include "bgiplayer/gpu_pixel_kernels.hpp"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace bgiplayer {
namespace {
const std::array<std::array<int16_t,256>,8>& periodic_weights() {
    // 14004C16B..4C224. Preserve the original double operation order and
    // truncation: rearranging the triangle-wave expression changes entries.
    static const auto weights=[] {
        std::array<std::array<int16_t,256>,8> result{};
        for(uint32_t mode=0;mode<8;++mode) {
            const double count=double(mode*2U+1U);
            const double period=256.0/count;
            const double frequency=count*0.00390625;
            for(uint32_t index=0;index<256;++index) {
                const int32_t band=static_cast<int32_t>(double(index)*frequency);
                // Force the original MULSD result to round before SUBSD;
                // an FMA would have a different rounding boundary.
                const volatile double band_origin=double(band)*period;
                const double remainder=double(index)-band_origin;
                const double phase=(band&1)?period-remainder:remainder;
                const auto value=static_cast<int32_t>((phase*256.0)*frequency);
                result[mode][index]=static_cast<int16_t>(uint16_t(value)<<4U);
            }
        }
        return result;
    }();
    return weights;
}
int32_t signed_floor_divide(int32_t value,int32_t divisor) {
    return value>=0?value/divisor:-((-value+divisor-1)/divisor);
}
}
bool compose_original_background_mask(GlyphBitmapView destination,
    GlyphBitmapView source, GlyphBitmapView mask, int32_t x, int32_t y,
    int32_t mask_left, int32_t mask_top, uint32_t mode, uint32_t opacity) {
    if (destination.format != 1U || source.format != 1U || mask.format != 3U ||
        destination.bytes_per_pixel != 4U || source.bytes_per_pixel != 4U ||
        mask.bytes_per_pixel != 1U || opacity > 256U) return false;
    const auto valid=[](const GlyphBitmapView& view) {
        const uint64_t row=uint64_t(view.width)*view.bytes_per_pixel;
        if (row>view.stride) return false;
        const uint64_t end=uint64_t(view.byte_offset)+
            (view.height?uint64_t(view.height-1U)*view.stride+row:0U);
        return end<=view.pixels.size();
    };
    if (!valid(destination)||!valid(source)||!valid(mask)) return false;
    // 14004C33B..347: 256 + (~(1<<mode))*opacity, with x86 dword wrap.
    const bool periodic=mode>=8U;
    const auto variant=mode&7U;
    const uint32_t bias=periodic?256U-2U*opacity:256U+~(1U<<variant)*opacity;
    const auto* weights=periodic?&periodic_weights()[variant]:nullptr;
    // Intersect the same destination/source/mask bounds once. Signed64
    // endpoints preserve INT32_MIN offsets without negation overflow.
    const int64_t left=std::max({int64_t{0},int64_t(x),-int64_t(mask_left)});
    const int64_t right=std::min({int64_t(destination.width),int64_t(x)+source.width,
        int64_t(mask.width)-mask_left});
    const int64_t top=std::max({int64_t{0},int64_t(y),-int64_t(mask_top)});
    const int64_t bottom=std::min({int64_t(destination.height),int64_t(y)+source.height,
        int64_t(mask.height)-mask_top});
    if(left>=right || top>=bottom)return true;
    if(fast_rendering_enabled()) {
        BackgroundMaskPixels pixels{destination,source,mask,
            uint32_t(left),uint32_t(top),uint32_t(right),uint32_t(bottom),
            uint32_t(left-x),uint32_t(top-y),uint32_t(left+mask_left),uint32_t(top+mask_top),
            {},periodic?12U:7U};
        for(uint32_t value=0;value<256;++value) {
            const int32_t threshold=static_cast<int32_t>((value<<(periodic?0U:variant))+bias);
            pixels.factors[value]=threshold<=0?0:threshold>=256?-1:
                periodic?(*weights)[threshold]:threshold>>1;
        }
        if(compose_host_fast_background_mask(pixels).success)return true;
    }

    for(int64_t dy=top;dy<bottom;++dy) {
        const auto* src=source.pixels.data()+source.byte_offset+size_t(dy-y)*source.stride+size_t(left-x)*4U;
        const auto* plane=mask.pixels.data()+mask.byte_offset+size_t(dy+mask_top)*mask.stride+size_t(left+mask_left);
        auto* dst=destination.pixels.data()+destination.byte_offset+size_t(dy)*destination.stride+size_t(left)*4U;
        for(int64_t dx=left;dx<right;++dx,src+=4,++plane,dst+=4) {
            const int32_t threshold=static_cast<int32_t>(
                (uint32_t(*plane)<<(periodic?0U:variant))+bias);
            if(threshold<=0)continue;
            if(threshold>=256) {std::copy_n(src,4,dst);continue;}
            const int32_t factor=periodic?(*weights)[threshold]:threshold>>1;
            for(size_t channel=0;channel<3;++channel) {
                const int32_t old=dst[channel];
                const int32_t delta=int32_t(src[channel])-old;
                const int32_t product=periodic?(delta*16)*factor:delta*factor;
                const int32_t correction=signed_floor_divide(product,periodic?65536:128);
                dst[channel]=static_cast<uint8_t>(std::clamp(old+correction,0,255));
            }
        }
    }
    return true;
}
}
