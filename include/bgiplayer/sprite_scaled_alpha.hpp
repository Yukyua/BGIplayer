#pragma once
#include <cstdint>
#include <span>
namespace bgiplayer {
// PE64 1400511A0 / 140051590, axis-aligned positive scales. Destination
// format1 is BGRX; source format2 is BGRA. Positions/origins/scales are Q16.
bool compose_scaled_alpha_bgra(
    std::span<uint8_t> destination, uint32_t destination_width,
    uint32_t destination_height, uint32_t destination_stride,
    std::span<const uint8_t> source, uint32_t source_width,
    uint32_t source_height, uint32_t source_stride,
    int32_t x_q16, int32_t y_q16, int32_t origin_x_q16, int32_t origin_y_q16,
    uint32_t scale_x_q16, uint32_t scale_y_q16, uint32_t parameter, bool filtered);
}
