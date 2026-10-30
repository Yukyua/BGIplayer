#pragma once
#include <cstdint>
#include "bgiplayer/pixel_storage.hpp"
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace bgiplayer {
struct CompressedBgV2Decoded {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t bits_per_pixel = 0;
    uint32_t format = 0;
    std::vector<uint8_t> pixels;
    // Optional native BGRX/BGRA allocation; only the resident entry point
    // requests this. The ordinary decoder still returns packed CPU bytes.
    std::shared_ptr<GpuPixelStorage> resident_pixels{};
};
// Original 004AE450 -> 004AAD80 still-image path. Unsupported transform
// and retained-block branches fail explicitly without partial output; 32-bit
// still images support original alpha modes1/2.
std::optional<CompressedBgV2Decoded> decode_compressed_bg_v2(
    std::span<const uint8_t> encoded, std::string* error = nullptr,
    bool allow_resident = false);
}  // namespace bgiplayer
