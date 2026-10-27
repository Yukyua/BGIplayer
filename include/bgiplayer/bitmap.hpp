#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace bgiplayer {

struct DecodedBitmap {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t format = 0;
    std::vector<uint8_t> rgba;
};

// Decodes the BGI bitmap header used by the target engine. Returns nullopt for
// unsupported packed/compressed variants rather than fabricating pixels.
std::optional<DecodedBitmap> decode_bgi_bitmap(
    std::span<const uint8_t> encoded);

}  // namespace bgiplayer
