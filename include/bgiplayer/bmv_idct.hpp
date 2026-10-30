#pragma once
#include <array>
#include <cstdint>
#include <span>

namespace bgiplayer {

// Original 004A9660/004A9750 multiply quantizer bytes by the 64 immutable
// float values at 00500A20. The array is in target coefficient order.
std::array<float,64> bmv_scaled_quantization(std::span<const uint8_t,64> quantization);

// Original 004AB5C0 separable SSE IDCT, expressed as independent float lanes.
// Output retains signed16 pack, arithmetic /8, +128 and byte saturation.
// False leaves output unchanged for invalid/non-finite caller quantization.
bool bmv_idct_block(const std::array<int16_t,64>& coefficients,
                    const std::array<float,64>& scaled_quantization,
                    std::array<int16_t,64>& samples);

// Immutable host-built original lookup bytes, shared with the GPU decoder.
const std::array<float,1024>& bmv_color_table_data();

// Original 004A9180 float lookup tables + 004AB5C0 output conversion.
// Input samples must be the IDCT's already-clamped 0..255 values.
bool bmv_ycbcr_to_bgr(int16_t y, int16_t cb, int16_t cr,
                      std::array<uint8_t,3>& bgr);

} // namespace bgiplayer
