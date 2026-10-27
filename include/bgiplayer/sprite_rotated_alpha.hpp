#pragma once
#include <cstdint>
#include <array>
#include <optional>
#include <span>
namespace bgiplayer {
std::optional<std::array<int32_t,6>> inverse_sprite_affine_q16(int32_t x_q16,int32_t y_q16,
    int32_t origin_x_q16,int32_t origin_y_q16,int32_t angle_q16,uint32_t scale_x_q16,uint32_t scale_y_q16);
// PE64 52700 inverse matrix + 511A0/51590 src2→dst1, positive scales.
bool compose_rotated_alpha_bgra(std::span<uint8_t> destination,uint32_t destination_width,
    uint32_t destination_height,uint32_t destination_stride,std::span<const uint8_t> source,
    uint32_t source_width,uint32_t source_height,uint32_t source_stride,int32_t x_q16,int32_t y_q16,
    int32_t origin_x_q16,int32_t origin_y_q16,int32_t angle_q16,uint32_t scale_x_q16,
    uint32_t scale_y_q16,uint32_t parameter,bool filtered);
}
