#include "bgiplayer/glyph.hpp"

#include <algorithm>
#include <limits>

namespace bgiplayer {
namespace {
bool fits_rows(std::size_t storage, uint64_t offset, uint32_t stride,
               uint32_t width_bytes, uint32_t height) {
    if (height == 0 || width_bytes == 0) {
        return offset <= storage;
    }
    return width_bytes <= stride &&
           offset + static_cast<uint64_t>(height - 1U) * stride + width_bytes <= storage;
}
void write16(std::span<uint8_t> bytes, std::size_t offset, uint16_t value) {
    bytes[offset] = static_cast<uint8_t>(value);
    bytes[offset + 1U] = static_cast<uint8_t>(value >> 8U);
}
void write32(std::span<uint8_t> bytes, std::size_t offset, uint32_t value) {
    write16(bytes, offset, static_cast<uint16_t>(value));
    write16(bytes, offset + 2U, static_cast<uint16_t>(value >> 16U));
}
} // namespace

std::optional<GlyphMask> load_glyph_outline(
    const GlyphOutlineProvider& provider, const GlyphFontRequest& font,
    uint32_t codepoint) {
    if (!provider || font.face.empty()) {
        return std::nullopt;
    }
    auto result = provider(font, codepoint);
    if (result && !fits_rows(result->alpha.size(), 0, result->stride,
                            result->width, result->height)) {
        return std::nullopt;
    }
    return result;
}

bool colorize_glyph_mask(
    GlyphBitmapView destination, const GlyphMask& glyph, uint32_t color) {
    if (destination.format > 2U) {
        return true;
    }
    if (glyph.character_bitmap) {
        if(destination.format!=2 || destination.bytes_per_pixel!=4 ||
           glyph.width>UINT32_MAX/4U || glyph.bitmap.size()!=uint64_t(glyph.width)*glyph.height*4U)
            return false;
        const uint32_t w=std::min(destination.width,glyph.width),h=std::min(destination.height,glyph.height);
        if(!fits_rows(destination.pixels.size(),destination.byte_offset,destination.stride,w*4U,h))return false;
        for(uint32_t y=0;y<h;++y)std::copy_n(glyph.bitmap.data()+size_t(y)*glyph.width*4U,
            size_t(w)*4U,destination.pixels.data()+destination.byte_offset+size_t(y)*destination.stride);
        return true;
    }
    const uint32_t width = std::min(destination.width, glyph.width);
    const uint32_t height = std::min(destination.height, glyph.height);
    const uint32_t pixel_size = destination.format == 0U ? 2U : 4U;
    if (width > std::numeric_limits<uint32_t>::max() / pixel_size ||
        destination.bytes_per_pixel != pixel_size ||
        !fits_rows(destination.pixels.size(), destination.byte_offset,
                   destination.stride, width * pixel_size, height) ||
        !fits_rows(glyph.alpha.size(), 0, glyph.stride, width, height)) {
        return false;
    }
    const uint32_t red = (color >> 16U) & 0xFFU;
    const uint32_t green = (color >> 8U) & 0xFFU;
    const uint32_t blue = color & 0xFFU;
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const uint32_t alpha = glyph.alpha[static_cast<std::size_t>(y) * glyph.stride + x];
            const std::size_t output = destination.byte_offset +
                static_cast<std::size_t>(y) * destination.stride +
                static_cast<std::size_t>(x) * pixel_size;
            if (destination.format == 2U) {
                write32(destination.pixels, output, (alpha << 24U) | (color & 0xFFFFFFU));
            } else if (destination.format == 1U) {
                write32(destination.pixels, output,
                        ((alpha * red & 0xFFFFFF00U) << 8U) |
                        (alpha * green & 0xFFFFFF00U) | ((alpha * blue) >> 8U));
            } else {
                const uint16_t value = static_cast<uint16_t>(
                    ((static_cast<uint16_t>(alpha * red >> 1U) & 0xFC1FU) |
                      static_cast<uint16_t>(alpha * green >> 6U)) & 0xFFE0U) |
                    static_cast<uint16_t>(alpha * blue >> 11U);
                write16(destination.pixels, output, value);
            }
        }
    }
    return true;
}

void trim_glyph_bitmap_view(GlyphBitmapView& bitmap, const GlyphMetrics& metrics) {
    const uint32_t available = bitmap.width - metrics.ink_left();
    const uint32_t ink_width = metrics.ink_right() - metrics.ink_left() + 1U;
    bitmap.width = std::min(available, ink_width);
    bitmap.byte_offset += bitmap.bytes_per_pixel * metrics.ink_left();
}
} // namespace bgiplayer
