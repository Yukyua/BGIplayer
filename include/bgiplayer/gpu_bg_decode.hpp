#pragma once
#include "bgiplayer/pixel_storage.hpp"
#include <array>
#include <cstdint>
#include <span>
#include <string_view>
namespace bgiplayer {
// Host preparation only. Coefficients retain original stripe-major Y/Cb/Cr
// block order; alpha, when present, uses the padded 8x8 tile dimensions.
struct BgDecodePixels {
    uint32_t width,height;
    std::span<const std::array<int16_t,64>> coefficients;
    std::span<const float,64> y_quant,c_quant;
    std::span<const uint8_t> alpha;
};
bool valid_gpu_bg_decode(const BgDecodePixels& pixels);
bool gpu_bg_decode_enabled(uint64_t pixels);
bool host_background_decode_available();
std::shared_ptr<GpuPixelStorage> decode_host_background(const BgDecodePixels& pixels);
namespace bg_decode_detail {
std::array<float,1152> tables(const BgDecodePixels& pixels);
std::string_view math_source();
std::string_view idct_source();
std::string_view color_source();
}
}
