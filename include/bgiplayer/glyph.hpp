#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace bgiplayer {

// Host boundary of 00432F90. The caller supplies the observed font descriptor;
// no substitute face, text shaping, layout, or advance is selected here.
struct GlyphFontRequest {
    std::u16string face;
    int32_t height = 0;
    int32_t width = 0;
    int32_t weight = 0;
    uint8_t italic = 0;
    uint8_t charset = 0;
    uint8_t quality = 0;
    uint32_t outline_gray_bits = 6; // GGO_GRAY8_BITMAP (64 levels).
};

// 004339C0 output has eleven dwords. Offsets +0x0C and +0x14 are
// inclusive ink bounds; +0x1C/+0x20/+0x24 are the ABC float values.
struct GlyphMetrics {
    std::array<uint32_t, 11> words{};
    [[nodiscard]] uint32_t ink_left() const { return words[3]; }
    [[nodiscard]] uint32_t ink_right() const { return words[5]; }
};

struct GlyphMask {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t stride = 0;
    std::vector<uint8_t> alpha;
    GlyphMetrics metrics;
    bool character_bitmap = false; // Original gaiji cache, not a host font outline.
    uint32_t source_format = 3;
    std::vector<uint8_t> bitmap; // Prepared original format-2 pixels; retain color/alpha.
    uint32_t character_bitmap_source_width = 0; // Measurement remains the registered width when scaled.
    bool character_bitmap_scaled = false;
};

using GlyphOutlineProvider = std::function<std::optional<GlyphMask>(
    const GlyphFontRequest&, uint32_t)>;

// A host may return unavailable. This seam never fabricates metrics or a face.
std::optional<GlyphMask> load_glyph_outline(
    const GlyphOutlineProvider& provider, const GlyphFontRequest& font,
    uint32_t codepoint);

// Six-dword target bitmap descriptor with a portable byte-offset instead of a
// 32-bit process pointer: pixels, row stride, width, height, format, pixel size.
struct GlyphBitmapView {
    std::span<uint8_t> pixels;
    uint32_t byte_offset = 0;
    uint32_t stride = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t format = 2;
    uint32_t bytes_per_pixel = 4;
};

// 00409E10: colorizes the intersection, preserving row padding and untouched
// pixels. False means the host storage/descriptor cannot represent the target
// access; no writes occur on failure. Formats other than 0/1/2 are target no-ops.
bool colorize_glyph_mask(
    GlyphBitmapView destination, const GlyphMask& glyph, uint32_t color);

// 0040A670: target uint32 subtraction/min and pointer arithmetic, including
// wraparound. This only changes the view; callers validate before memory use.
void trim_glyph_bitmap_view(GlyphBitmapView& bitmap, const GlyphMetrics& metrics);

} // namespace bgiplayer
