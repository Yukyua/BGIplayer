#pragma once
#include <cstdint>
#include <span>
namespace bgiplayer {
// Original00411B30/140046F30 format0, RGB555 with the color0/weight0 raw-copy arm.
bool filter_rgb555_color(std::span<uint8_t> destination,uint32_t stride,
    std::span<const uint8_t> source,uint32_t source_stride,uint32_t width,uint32_t height,
    uint32_t color,uint32_t weight);
} // namespace bgiplayer
