#pragma once
#include <cstdint>
#include <span>
namespace bgiplayer {
// PE32 00412150, format1 destination and a format3 byte mask view.
bool filter_bitmap_mask(std::span<uint8_t> destination,uint32_t stride,
    std::span<const uint8_t> mask,uint32_t mask_stride,uint32_t width,uint32_t height,
    uint32_t color,uint32_t shift,uint32_t weight);
} // namespace bgiplayer
