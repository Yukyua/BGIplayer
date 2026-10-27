#include "bgiplayer/bitmap.hpp"

#include <cstddef>

namespace bgiplayer {
namespace {

uint16_t read_u16(std::span<const uint8_t> data, std::size_t offset) {
    return static_cast<uint16_t>(
        static_cast<uint16_t>(data[offset]) |
        static_cast<uint16_t>(
            static_cast<uint16_t>(data[offset + 1]) << 8U));
}

}  // namespace

std::optional<DecodedBitmap> decode_bgi_bitmap(
    std::span<const uint8_t> encoded) {
    if (encoded.size() < 16) {
        return std::nullopt;
    }
    const uint16_t width = read_u16(encoded, 0);
    const uint16_t height = read_u16(encoded, 2);
    const uint16_t bits_per_pixel = read_u16(encoded, 4);
    const uint16_t compression = read_u16(encoded, 6);
    if (width == 0 || height == 0 || compression != 0) {
        return std::nullopt;
    }
    const std::size_t pixels =
        static_cast<std::size_t>(width) * height;
    if (pixels > 0x04000000U) {
        return std::nullopt;
    }
    DecodedBitmap result;
    result.width = width;
    result.height = height;
    result.format = bits_per_pixel;
    result.rgba.resize(pixels * 4);
    const auto* source = encoded.data() + 16;
    const std::size_t source_size = encoded.size() - 16;
    if (bits_per_pixel == 24) {
        if (source_size < pixels * 3) {
            return std::nullopt;
        }
        for (std::size_t index = 0; index < pixels; ++index) {
            result.rgba[index * 4] = source[index * 3 + 2];
            result.rgba[index * 4 + 1] = source[index * 3 + 1];
            result.rgba[index * 4 + 2] = source[index * 3];
            result.rgba[index * 4 + 3] = 0xFF;
        }
        return result;
    }
    if (bits_per_pixel == 32) {
        if (source_size < pixels * 4) {
            return std::nullopt;
        }
        for (std::size_t index = 0; index < pixels; ++index) {
            result.rgba[index * 4] = source[index * 4 + 2];
            result.rgba[index * 4 + 1] = source[index * 4 + 1];
            result.rgba[index * 4 + 2] = source[index * 4];
            result.rgba[index * 4 + 3] = source[index * 4 + 3];
        }
        return result;
    }
    return std::nullopt;
}

}  // namespace bgiplayer
