#include "bgiplayer/effector_selectors.hpp"
#include <algorithm>
#include <array>
#include <cstring>

namespace bgiplayer {
namespace {
bool valid(const GlyphBitmapView& v) {
    return (v.format == 1 || v.format == 2) && v.bytes_per_pixel == 4 &&
           v.width && v.height && uint64_t(v.stride) >= uint64_t(v.width) * 4 &&
           uint64_t(v.byte_offset) + uint64_t(v.height - 1) * v.stride +
               uint64_t(v.width) * 4 <= v.pixels.size();
}
void axis(GlyphBitmapView dst, GlyphBitmapView src, uint32_t radius,
          bool vertical, bool clamp_edges) {
    const uint32_t diameter = radius * 2 + 1;
    const uint32_t reciprocal = ((0x20000U + diameter * 2) / diameter) & 0xffffU;
    const uint32_t lines = vertical ? src.width : src.height;
    const uint32_t length = vertical ? src.height : src.width;
    for (uint32_t line = 0; line < lines; ++line) {
        std::array<uint32_t, 4> sums{};
        const auto add = [&](int64_t position, int sign) {
            if (!clamp_edges && (position < 0 || uint64_t(position) >= length)) return;
            position = std::clamp<int64_t>(position, 0, length - 1);
            const uint64_t offset = src.byte_offset +
                (vertical ? uint64_t(position) * src.stride + uint64_t(line) * 4 :
                            uint64_t(line) * src.stride + uint64_t(position) * 4);
            for (unsigned channel = 0; channel < 4; ++channel)
                sums[channel] += sign * int32_t(src.pixels[offset + channel] >> 1);
        };
        for (int64_t position = -int64_t(radius); position <= radius; ++position)
            add(position, 1);
        for (uint32_t position = 0; position < length; ++position) {
            const uint64_t offset = dst.byte_offset +
                (vertical ? uint64_t(position) * dst.stride + uint64_t(line) * 4 :
                            uint64_t(line) * dst.stride + uint64_t(position) * 4);
            for (unsigned channel = 0; channel < 4; ++channel)
                dst.pixels[offset + channel] = uint8_t((sums[channel] * reciprocal) >> 16);
            add(int64_t(position) - radius, -1);
            add(int64_t(position) + radius + 1, 1);
        }
    }
}
}
bool blur_pe64_effector_selector(GlyphBitmapView dst, GlyphBitmapView src,
                                    uint32_t radius, uint32_t selector) {
    if (!valid(dst) || !valid(src) || dst.format != src.format ||
        dst.width != src.width || dst.height != src.height ||
        selector > 5 || radius > 256) return false;
    // 499A0/493F0 radius0 takes 3E440; 48D80 allocates but both passes copy.
    if (!radius) {
        for (uint32_t row = 0; row < src.height; ++row)
            std::memmove(dst.pixels.data() + dst.byte_offset + uint64_t(row) * dst.stride,
                         src.pixels.data() + src.byte_offset + uint64_t(row) * src.stride,
                         uint64_t(src.width) * 4);
        return true;
    }
    radius = std::min(radius, 255U);
    const bool clamp_edges = selector & 1U;
    if (selector < 4) {
        axis(dst, src, radius, selector >= 2, clamp_edges);
        return true;
    }
    std::vector<uint8_t> temporary(uint64_t(src.width) * src.height * 4);
    GlyphBitmapView mid{temporary, 0, src.width * 4, src.width, src.height, src.format, 4};
    axis(mid, src, radius, false, clamp_edges);
    axis(dst, mid, radius, true, clamp_edges);
    return true;
}
}
