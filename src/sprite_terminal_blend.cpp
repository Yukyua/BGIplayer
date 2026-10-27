#include "bgiplayer/sprite_terminal_blend.hpp"
#include <algorithm>
namespace bgiplayer {
bool blend_original_sprite_terminal(GlyphBitmapView destination,
                                    GlyphBitmapView source,
                                    uint32_t mode, uint32_t parameter) {
    if ((mode != 2U && mode != 3U && mode != 0x21U && mode != 0x22U) ||
        parameter > 256U || destination.format != 1U || source.format != 2U ||
        destination.bytes_per_pixel != 4U || source.bytes_per_pixel != 4U ||
        destination.width != source.width || destination.height != source.height)
        return false;
    const auto valid = [](GlyphBitmapView view) {
        return view.width > 0U && view.height > 0U &&
            view.stride >= uint64_t(view.width) * 4U &&
            uint64_t(view.byte_offset) + uint64_t(view.stride) * (view.height - 1U) +
                uint64_t(view.width) * 4U <= view.pixels.size();
    };
    if (!valid(destination) || !valid(source)) return false;
    const uint32_t strength = mode >= 0x20U ? 256U - parameter : parameter;
    const bool subtract = mode == 3U || mode == 0x22U;
    for (uint32_t y = 0; y < source.height; ++y)
        for (uint32_t x = 0; x < source.width; ++x) {
            const size_t input = source.byte_offset + size_t(y) * source.stride + size_t(x) * 4U;
            const size_t output = destination.byte_offset + size_t(y) * destination.stride + size_t(x) * 4U;
            // Original local table truncates (index*strength)>>8. At256,
            // the original startup table has RGB=index, fourth lane=0.
            const uint32_t weight = ((source.pixels[input + 3U] >> 1U) * strength) >> 8U;
            for (size_t channel = 0; channel < 3U; ++channel) {
                const uint32_t delta = (source.pixels[input + channel] * weight) >> 7U;
                const uint32_t old = destination.pixels[output + channel];
                destination.pixels[output + channel] = uint8_t(subtract ?
                    (old > delta ? old - delta : 0U) : std::min(255U, old + delta));
            }
        }
    return true;
}
}
