#pragma once
#include "bgiplayer/glyph.hpp"
#include <filesystem>

namespace bgiplayer {

// Values recovered from 00432AE0/00432F90 font objects. Raster size belongs to
// the host API; logical size, canvas and crop remain target geometry.
struct FreeTypeGlyphConfiguration {
    std::filesystem::path font_file;
    int64_t face_index;
    int32_t logical_height; // target font+8
    int32_t width_percent; // target font+0x0C
    int32_t crop_x; // target font+0x20
    int32_t crop_y; // target font+0x24
    uint32_t pixel_width; // explicit FT size, 0 preserves font aspect
    uint32_t pixel_height;
    uint32_t metric_divisor; // target 00529B80, at least 1
};

struct TargetGrayOutline {
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    std::span<const uint8_t> coverage; // GetGlyphOutline gray levels 0..2^bits
    int32_t host_ascent;
    int32_t glyph_origin_y;
    float abc_a;
    float abc_b;
    float abc_c;
    int32_t text_extent_x;
};

// Pure target part of 00432F90; independent fixtures can supply GDI output.
std::optional<GlyphMask> map_target_gray_outline(
    const FreeTypeGlyphConfiguration& configuration, uint32_t codepoint,
    uint32_t gray_bits, const TargetGrayOutline& outline);

// No system font lookup or fallback. Provider owns FT library/face lifetime.
// FreeType substitutes the OS rasterizer only; masks may differ from GDI until
// paired runtime coverage fixtures are compared. Empty return means unavailable.
std::optional<GlyphOutlineProvider> make_freetype_glyph_provider(
    const FreeTypeGlyphConfiguration& configuration);

} // namespace bgiplayer
