#pragma once
#include <cstdint>
#include <span>
namespace bgiplayer {
// PE32 00411800 ->00411830/00411990. Mode2 luminance tint with the
// original 16-bit lane rounding and format2 paired-alpha zero shortcut.
bool filter_luminance_color(std::span<uint8_t> destination,
    uint32_t destination_stride, std::span<const uint8_t> source,
    uint32_t source_stride, uint32_t width, uint32_t height,
    uint32_t format, uint32_t color, uint32_t weight);
// PE32 0040EA60 ->0040ED50, format2 black source-over filter.
bool filter_black_alpha(std::span<uint8_t> destination, uint32_t destination_stride,
    std::span<const uint8_t> source, uint32_t source_stride,
    uint32_t width, uint32_t height, uint32_t weight);
// PE32 004114D0, XOR RGB lanes with original strength quantization.
bool filter_xor_color(std::span<uint8_t> destination, uint32_t destination_stride,
    std::span<const uint8_t> source, uint32_t source_stride,
    uint32_t width, uint32_t height, uint32_t format, uint32_t color, uint32_t weight);
// PE32 filter mode1: color zero subtracts white; any other word adds RGB.
bool filter_addsub_color(std::span<uint8_t> destination, uint32_t destination_stride,
    std::span<const uint8_t> source, uint32_t source_stride,
    uint32_t width, uint32_t height, uint32_t format, uint32_t color, uint32_t weight);
} // namespace bgiplayer
