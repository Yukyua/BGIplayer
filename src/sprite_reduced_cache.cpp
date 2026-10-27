#include "bgiplayer/sprite_reduced_cache.hpp"
#include <algorithm>
namespace bgiplayer {
bool reduce_original_sprite_cache(GlyphBitmapView destination,
                                  GlyphBitmapView source) {
    if ((source.format != 1U && source.format != 2U) ||
        destination.format != source.format || source.bytes_per_pixel != 4U ||
        destination.bytes_per_pixel != 4U || source.width < 2U || source.height < 2U ||
        destination.width != (source.width + 1U) / 2U ||
        destination.height != (source.height + 1U) / 2U ||
        source.stride < uint64_t(source.width) * 4U ||
        destination.stride < uint64_t(destination.width) * 4U ||
        uint64_t(source.byte_offset) + uint64_t(source.stride) * (source.height - 1U) +
            uint64_t(source.width) * 4U > source.pixels.size() ||
        uint64_t(destination.byte_offset) + uint64_t(destination.stride) * (destination.height - 1U) +
            uint64_t(destination.width) * 4U > destination.pixels.size())
        return false;
    const auto average = [](unsigned a, unsigned b) { return (a + b + 1U) >> 1U; };
    for (uint32_t y = 0; y < destination.height; ++y) {
        const uint32_t top = y * 2U;
        const uint32_t bottom = std::min(top + 1U, source.height - 1U);
        for (uint32_t x = 0; x < destination.width; ++x) {
            const uint32_t left = x * 2U;
            const uint32_t right = std::min(left + 1U, source.width - 1U);
            const auto sample = [&](uint32_t sx, uint32_t sy, unsigned channel) {
                return source.pixels[source.byte_offset + size_t(sy) * source.stride + size_t(sx) * 4U + channel];
            };
            for (unsigned channel = 0; channel < 4U; ++channel)
                destination.pixels[destination.byte_offset + size_t(y) * destination.stride + size_t(x) * 4U + channel] =
                    uint8_t(average(average(sample(left, top, channel), sample(left, bottom, channel)),
                                    average(sample(right, top, channel), sample(right, bottom, channel))));
        }
    }
    return true;
}
}
