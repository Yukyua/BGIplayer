#include "bgiplayer/glyph_freetype.hpp"
#include "bgiplayer/text_layout.hpp"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_SYNTHESIS_H

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <memory>
#include <mutex>

namespace bgiplayer {
namespace {
struct FreeTypeState {
    FT_Library library = nullptr;
    FT_Face face = nullptr;
    std::mutex mutex;
    ~FreeTypeState() {
        if (face) FT_Done_Face(face);
        if (library) FT_Done_FreeType(library);
    }
};
bool valid_configuration(const FreeTypeGlyphConfiguration& config) {
    return config.logical_height > 0 && config.logical_height <= 0x1000 &&
           config.width_percent > 0 && config.width_percent <= 0x1000 &&
           config.pixel_height > 0 && config.pixel_height <= 0x1000 &&
           config.pixel_width <= 0x1000 && config.metric_divisor > 0 &&
           config.metric_divisor <= 0x1000;
}
uint32_t float_word(float value) { return std::bit_cast<uint32_t>(value); }
}

std::optional<GlyphMask> map_target_gray_outline(
    const FreeTypeGlyphConfiguration& config, uint32_t codepoint,
    uint32_t gray_bits, const TargetGrayOutline& outline) {
    if (!valid_configuration(config) || (gray_bits != 2 && gray_bits != 4 && gray_bits != 6) ||
        outline.stride < outline.width ||
        (outline.height != 0 && outline.width != 0 &&
         static_cast<uint64_t>(outline.height - 1U) * outline.stride + outline.width > outline.coverage.size()) ||
        !std::isfinite(outline.abc_a) || !std::isfinite(outline.abc_b) || !std::isfinite(outline.abc_c)) {
        return std::nullopt;
    }
    const auto representable_metric = [&](double value) {
        const double scaled = value / config.metric_divisor;
        return std::isfinite(scaled) && scaled >= std::numeric_limits<int32_t>::min() &&
               scaled <= std::numeric_limits<int32_t>::max();
    };
    if (!representable_metric(outline.abc_a) || !representable_metric(outline.abc_b) ||
        !representable_metric(outline.abc_c) ||
        !representable_metric(static_cast<double>(outline.abc_a) + outline.abc_b + outline.abc_c)) {
        return std::nullopt;
    }
    GlyphMask result;
    result.width = static_cast<uint32_t>((config.logical_height * config.width_percent * 2) / 100);
    result.height = static_cast<uint32_t>((config.logical_height * 3) / 2);
    result.stride = result.width;
    if (result.width == 0 || result.height == 0) return std::nullopt;
    result.alpha.assign(static_cast<std::size_t>(result.stride) * result.height, 0);
    const int64_t top = std::max<int64_t>(0, static_cast<int64_t>(outline.host_ascent) - config.crop_y - outline.glyph_origin_y);
    // Target ignores 1x1 outline boxes, and GGO's x origin is not added here.
    if ((outline.width > 1 || outline.height > 1) && outline.width > 0 && outline.height > 0) {
        const uint32_t rows = std::min(result.height, outline.height);
        const uint32_t columns = std::min(result.width, outline.width);
        for (uint32_t y = 0; y < rows; ++y) {
            const int64_t output_y = top + y;
            if (output_y < 0 || output_y >= result.height) continue;
            for (uint32_t x = 0; x < columns; ++x) {
                const int64_t output_x = static_cast<int64_t>(config.crop_x) + x;
                if (output_x < 0 || output_x >= result.width) continue;
                const uint32_t gray = outline.coverage[static_cast<std::size_t>(y) * outline.stride + x];
                result.alpha[static_cast<std::size_t>(output_y) * result.stride + static_cast<std::size_t>(output_x)] =
                    static_cast<uint8_t>((gray * 255U) >> gray_bits);
            }
        }
    }
    auto& words = result.metrics.words;
    words[0] = codepoint;
    words[1] = target_wide_character(codepoint) ? 1U : 0U;
    words[4] = 0;
    words[6] = result.height - 1U;
    const bool space = codepoint == 0x20 || codepoint == 0x3000;
    if (space) {
        words[3] = 0;
        words[5] = static_cast<uint32_t>(static_cast<int32_t>(
            (outline.abc_a + outline.abc_b + outline.abc_c) / static_cast<float>(config.metric_divisor))) - 1U;
    } else {
        bool found = false;
        uint32_t left = 0;
        uint32_t right = 0;
        for (uint32_t x = 0; x < result.width; ++x) {
            bool ink = false;
            for (uint32_t y = 0; y < result.height; ++y) {
                ink = ink || result.alpha[static_cast<std::size_t>(y) * result.stride + x] != 0;
            }
            if (ink) {
                if (!found) left = x;
                right = x;
                found = true;
            }
        }
        words[3] = left;
        words[5] = found ? right : static_cast<uint32_t>(config.logical_height / 2) - 1U;
    }
    const auto target_abc = [&](float value) {
        return static_cast<float>(static_cast<int32_t>(value / static_cast<float>(config.metric_divisor)));
    };
    words[7] = float_word(space ? 0.0F : target_abc(outline.abc_a));
    words[8] = float_word(space ? static_cast<float>(words[5] + 1U) : target_abc(outline.abc_b));
    words[9] = float_word(space ? 0.0F : target_abc(outline.abc_c));
    // Host text_extent_x is already divided to logical pixels by the provider.
    words[10] = target_wide_character(codepoint) &&
        ((codepoint > 0xFF00 && codepoint < 0xFF5F) || config.logical_height / 2 < outline.text_extent_x) ? 1U : 0U;
    return result;
}

std::optional<GlyphOutlineProvider> make_freetype_glyph_provider(
    const FreeTypeGlyphConfiguration& config) {
    if (!valid_configuration(config) || config.font_file.empty()) return std::nullopt;
    auto state = std::make_shared<FreeTypeState>();
    if (FT_Init_FreeType(&state->library) != 0 ||
        FT_New_Face(state->library, config.font_file.c_str(), static_cast<FT_Long>(config.face_index), &state->face) != 0 ||
        FT_Select_Charmap(state->face, FT_ENCODING_UNICODE) != 0 ||
        FT_Set_Pixel_Sizes(state->face, config.pixel_width, config.pixel_height) != 0) return std::nullopt;
    GlyphOutlineProvider provider = [state, config](const GlyphFontRequest& request, uint32_t codepoint) -> std::optional<GlyphMask> {
        if (request.face.empty() || (request.outline_gray_bits != 2 && request.outline_gray_bits != 4 && request.outline_gray_bits != 6)) {
            return std::nullopt;
        }
        std::lock_guard lock(state->mutex);
        const FT_UInt index = FT_Get_Char_Index(state->face, codepoint);
        // Original 004333CF/00433406 asks GDI for the requested Unicode glyph.
        // GDI returns the selected face's default glyph when its cmap misses.
        // FreeType exposes that same face-owned default outline at index zero.
        if (FT_Load_Glyph(state->face, index, FT_LOAD_DEFAULT | FT_LOAD_TARGET_NORMAL) != 0) return std::nullopt;
        if (request.weight >= 700 && (state->face->style_flags & FT_STYLE_FLAG_BOLD) == 0) FT_GlyphSlot_Embolden(state->face->glyph);
        if (request.italic != 0 && (state->face->style_flags & FT_STYLE_FLAG_ITALIC) == 0) FT_GlyphSlot_Oblique(state->face->glyph);
        if (FT_Render_Glyph(state->face->glyph, FT_RENDER_MODE_NORMAL) != 0) return std::nullopt;
        const FT_GlyphSlot slot = state->face->glyph;
        const FT_Bitmap& bitmap = slot->bitmap;
        if (bitmap.width != 0 && bitmap.rows != 0 && bitmap.pixel_mode != FT_PIXEL_MODE_GRAY) return std::nullopt;
        std::vector<uint8_t> gray(static_cast<std::size_t>(bitmap.width) * bitmap.rows);
        const uint32_t levels = 1U << request.outline_gray_bits;
        for (uint32_t y = 0; y < bitmap.rows; ++y) {
            const auto row = bitmap.pitch >= 0 ? bitmap.buffer + static_cast<std::size_t>(y) * static_cast<uint32_t>(bitmap.pitch)
                : bitmap.buffer + static_cast<std::size_t>(bitmap.rows - 1U - y) * static_cast<uint32_t>(-bitmap.pitch);
            for (uint32_t x = 0; x < bitmap.width; ++x) {
                gray[static_cast<std::size_t>(y) * bitmap.width + x] =
                    static_cast<uint8_t>((static_cast<uint32_t>(row[x]) * levels + 127U) / 255U);
            }
        }
        const float a = static_cast<float>(slot->bitmap_left);
        const float b = static_cast<float>(bitmap.width);
        const float advance = static_cast<float>(slot->advance.x) / 64.0F;
        const auto ascent = static_cast<int32_t>((state->face->size->metrics.ascender + 63) / 64);
        TargetGrayOutline outline{bitmap.width, bitmap.rows, bitmap.width, gray, ascent, slot->bitmap_top,
            a, b, advance - a - b, static_cast<int32_t>(advance / static_cast<float>(config.metric_divisor))};
        return map_target_gray_outline(config, codepoint, request.outline_gray_bits, outline);
    };
    return provider;
}
} // namespace bgiplayer
