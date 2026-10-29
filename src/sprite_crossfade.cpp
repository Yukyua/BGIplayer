#include "bgiplayer/sprite_crossfade.hpp"
#include "bgiplayer/gpu_exact_pixel_ops.hpp"
#include "bgiplayer/rcpss.hpp"
#include <algorithm>
#include <array>
namespace bgiplayer {
namespace {
int32_t signed_shift8(int32_t value) {
    return value >= 0 ? value / 256 : -((-value + 255) / 256);
}
// 1400416A0 -> 14003C5C0 uses the original RCPPS estimate, not integer
// division. Pack the already captured estimate table without changing a bit:
// each adjacent pair is identical, and successive 11-bit units differ by1..4.
// Six independent entries fit in a dword (12-bit base, five3-bit deltas).
constexpr auto packed_sprite_reciprocals() {
    std::array<uint32_t,342> result{};
    for (size_t block=0;block<result.size();++block) {
        const size_t first=block*6U;
        result[block]=(detail::kRcpSsEstimateTable[first*2U]-0x3F000000U)>>11U;
        for (size_t i=1;i<6U && first+i<2048U;++i)
            result[block]|=((detail::kRcpSsEstimateTable[(first+i-1U)*2U]-
                detail::kRcpSsEstimateTable[(first+i)*2U])>>11U)<<(12U+(i-1U)*3U);
    }
    return result;
}
constexpr auto sprite_reciprocals=packed_sprite_reciprocals();
}
bool blend_original_sprite_crossfade(GlyphBitmapView destination,
    GlyphBitmapView first, GlyphBitmapView second, int32_t mix, uint32_t opacity) {
    if (destination.format != 1U || first.format != 2U || second.format != 2U ||
        mix < 0 || mix > 256) return false;
    if (opacity >= 256U) return true;
    const uint32_t width = std::min(first.width,second.width);
    const uint32_t height = std::min(first.height,second.height);
    if (destination.width < width || destination.height < height) return false;
    for (const auto& view : {destination,first,second}) {
        if (view.bytes_per_pixel != 4U || view.stride < static_cast<uint64_t>(width)*4U ||
            view.byte_offset > view.pixels.size() ||
            (width != 0U && height != 0U &&
             static_cast<uint64_t>(height-1U)*view.stride+static_cast<uint64_t>(width)*4U >
                 view.pixels.size()-view.byte_offset))
            return false;
    }
    for (uint32_t row=0;row<height;++row) for (uint32_t column=0;column<width;column+=2U) {
        const uint32_t count=std::min(2U,width-column);
        std::array<std::array<int32_t,4>,2> components{};
        for (uint32_t pixel=0;pixel<count;++pixel) {
            const std::size_t a=first.byte_offset+static_cast<std::size_t>(row)*first.stride+(column+pixel)*4U;
            const std::size_t b=second.byte_offset+static_cast<std::size_t>(row)*second.stride+(column+pixel)*4U;
            const int32_t alpha_a=static_cast<int32_t>((first.pixels[a+3U]*(256U-opacity))>>8U);
            const int32_t alpha_b=static_cast<int32_t>((second.pixels[b+3U]*(256U-opacity))>>8U);
            for (uint32_t channel=0;channel<4U;++channel) {
                const int32_t ca=channel==3U ? alpha_a : (first.pixels[a+channel]*alpha_a)>>8;
                const int32_t cb=channel==3U ? alpha_b : (second.pixels[b+channel]*alpha_b)>>8;
                components[pixel][channel]=ca+signed_shift8((cb-ca)*mix);
            }
        }
        // CF20's pair tests both alpha words; its odd tail tests one.
        if (components[0][3]==0 && (count==1U || components[1][3]==0)) continue;
        for (uint32_t pixel=0;pixel<count;++pixel) {
            const std::size_t out=destination.byte_offset+static_cast<std::size_t>(row)*destination.stride+(column+pixel)*4U;
            const int32_t alpha=components[pixel][3];
            for (uint32_t channel=0;channel<3U;++channel)
                destination.pixels[out+channel]=static_cast<uint8_t>(std::clamp(
                    components[pixel][channel]+((destination.pixels[out+channel]*(256-alpha))>>8),0,255));
            destination.pixels[out+3U]=0;
        }
    }
    return true;
}

bool blend_original_sprite_cache(GlyphBitmapView destination,
    GlyphBitmapView first, GlyphBitmapView second, int32_t mix) {
    if (destination.format != 2U || first.format != 2U || second.format != 2U ||
        mix < 0 || mix > 256) return false;
    const uint32_t width=std::min(first.width,second.width);
    const uint32_t height=std::min(first.height,second.height);
    if (destination.width<width || destination.height<height) return false;
    for (const auto& view : {destination,first,second})
        if (view.bytes_per_pixel!=4U || view.stride<uint64_t(width)*4U ||
            view.byte_offset>view.pixels.size() ||
            (width && height && uint64_t(height-1U)*view.stride+uint64_t(width)*4U>
                view.pixels.size()-view.byte_offset)) return false;
    if (exact_pixels_worthwhile(width,height)) {
    ExactPixelPixels gpu{destination,first,second,ExactPixelOperation::SpritePairMix};
    gpu.parameters[0]=uint32_t(mix);
    std::copy(sprite_reciprocals.begin(),sprite_reciprocals.end(),gpu.parameters.begin()+32);
    if (compose_host_exact_pixels(gpu).success) return true;
    }
    for (uint32_t y=0;y<height;++y) for (uint32_t x=0;x<width;++x) {
        const size_t a=first.byte_offset+size_t(y)*first.stride+x*4U;
        const size_t b=second.byte_offset+size_t(y)*second.stride+x*4U;
        const size_t d=destination.byte_offset+size_t(y)*destination.stride+x*4U;
        const uint32_t aw=first.pixels[a+3U]*uint32_t(256-mix);
        const uint32_t bw=second.pixels[b+3U]*uint32_t(mix);
        const uint32_t sum=aw+bw;
        // Original RCPPS then MULPS then CVTTPS2DQ, followed by its 129-entry
        // coefficient table. Preserve the approximate reciprocal at endpoints.
        const float reciprocal=detail::rcpss_estimate(float(sum?sum:1U));
        const int weight=static_cast<int>(reciprocal*float(aw<<7U));
        for (unsigned c=0;c<3;++c) {
            const int delta=int(first.pixels[a+c])-int(second.pixels[b+c]);
            const int product=delta*weight;
            const int step=product>=0?product/128:-((-product+127)/128);
            destination.pixels[d+c]=static_cast<uint8_t>(std::clamp(
                int(second.pixels[b+c])+step,0,255));
        }
        destination.pixels[d+3U]=static_cast<uint8_t>(sum>>8U);
    }
    return true;
}
}
