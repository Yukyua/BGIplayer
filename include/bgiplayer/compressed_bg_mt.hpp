#pragma once
#include "bgiplayer/compressed_bg_v2.hpp"
namespace bgiplayer {
// PE64 1400C02A0, sequential host execution of independent original stripes.
std::optional<CompressedBgV2Decoded> decode_compressed_bg_mt(
    std::span<const uint8_t> encoded, std::string* error = nullptr);
}
