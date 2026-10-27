#include "bgiplayer/background_transform.hpp"
#include "bgiplayer/gpu_pixel_kernels.hpp"
#include "bgiplayer/sprite_rotated_alpha.hpp"

#include <algorithm>

#include <cmath>
#include <limits>

namespace bgiplayer {
namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kAngleDivisor = 11796480.0;  // 180 << 16
constexpr double kFixedOne = 65536.0;

int32_t original_truncate(double value) {
    // 00419AE0 uses CVTTSD2SI and therefore saturates out-of-range values to
    // the integer indefinite value instead of wrapping.
    if (!std::isfinite(value) || value <= -2147483649.0 ||
        value >= 2147483648.0)
        return std::numeric_limits<int32_t>::min();
    return static_cast<int32_t>(value);
}

int32_t source_byte(const uint8_t* source, uint32_t stride, uint32_t width,
                    uint32_t height, int32_t x, int32_t y, uint32_t channel) {
    if (x < 0 || y < 0 || static_cast<uint32_t>(x) >= width ||
        static_cast<uint32_t>(y) >= height)
        return 0;
    return source[static_cast<std::size_t>(y) * stride +
                  static_cast<std::size_t>(x) * 4U + channel];
}
}  // namespace

std::optional<std::pair<int32_t,int32_t>> classify_background_exact_copy(
    const BackgroundTransform& transform, int32_t destination_x,
    int32_t destination_y, uint32_t destination_width,
    uint32_t destination_height, uint32_t source_width, uint32_t source_height) {
    if (((destination_x | destination_y | transform.origin_x | transform.origin_y)&0xFFFF)!=0 ||
        transform.angle % 0x1680000 != 0 || transform.scale_x != 0x10000 ||
        transform.scale_y != 0x10000 || destination_width==0 || destination_height==0)
        return std::nullopt;
    const int32_t x=(transform.origin_x>>16)-(destination_x>>16);
    const int32_t y=(transform.origin_y>>16)-(destination_y>>16);
    if(x<0 || y<0 || uint64_t(x)+destination_width>source_width ||
        uint64_t(y)+destination_height>source_height) return std::nullopt;
    return std::pair{x,y};
}

std::optional<BackgroundAffine> evaluate_background_affine(
    const BackgroundTransform& transform, int32_t destination_x,
    int32_t destination_y) {
    if (transform.scale_x <= 0 || transform.scale_y <= 0) return std::nullopt;
    const double radians =
        static_cast<double>(transform.angle) * kPi / kAngleDivisor;
    const double sine = std::sin(radians);
    const double cosine = std::cos(radians);
    const double inverse_x = kFixedOne / static_cast<double>(transform.scale_x);
    const double inverse_y = kFixedOne / static_cast<double>(transform.scale_y);
    const double dx = static_cast<double>(destination_x) / kFixedOne;
    const double dy = static_cast<double>(destination_y) / kFixedOne;
    const double ox = static_cast<double>(transform.origin_x) / kFixedOne;
    const double oy = static_cast<double>(transform.origin_y) / kFixedOne;
    BackgroundAffine affine;
    affine.start_x =
        original_truncate((ox - (dx * cosine - dy * sine) * inverse_x) *
                          kFixedOne);
    affine.start_y =
        original_truncate((oy - (dx * sine + dy * cosine) * inverse_y) *
                          kFixedOne);
    affine.column_x = original_truncate(cosine * inverse_x * kFixedOne);
    affine.column_y = original_truncate(sine * inverse_y * kFixedOne);
    affine.row_x = original_truncate(-sine * inverse_x * kFixedOne);
    affine.row_y = original_truncate(cosine * inverse_y * kFixedOne);
    return affine;
}

std::optional<BackgroundAffine> evaluate_pe64_background_affine(
    const BackgroundTransform& transform, int32_t destination_x,
    int32_t destination_y) {
    if (transform.scale_x <= 0 || transform.scale_y <= 0) return std::nullopt;
    const auto matrix = inverse_sprite_affine_q16(destination_x, destination_y,
        transform.origin_x, transform.origin_y, transform.angle,
        static_cast<uint32_t>(transform.scale_x),
        static_cast<uint32_t>(transform.scale_y));
    if (!matrix) return std::nullopt;
    return BackgroundAffine{(*matrix)[0], (*matrix)[1], (*matrix)[2],
                            (*matrix)[3], (*matrix)[4], (*matrix)[5]};
}

bool sample_background_transform_nearest(
    uint8_t* destination, uint32_t destination_stride, uint32_t width,
    uint32_t height, const uint8_t* source, uint32_t source_stride,
    uint32_t source_width, uint32_t source_height,
    const BackgroundAffine& affine) {
    // 14004FE80 packs integer coordinates into signed16 lanes and its source
    // pitch into signed16 pixel units. Keep larger layouts explicitly gated.
    if (!destination || !source || width == 0 || height == 0 ||
        source_width == 0 || source_height == 0 || source_width > 32767U ||
        source_height > 32767U || source_stride > 131068U ||
        destination_stride % 4U || source_stride % 4U ||
        destination_stride < uint64_t(width) * 4U ||
        source_stride < uint64_t(source_width) * 4U)
        return false;
    if(compose_host_fast_affine({{destination,size_t(destination_stride)*height},width,height,destination_stride,
            {source,size_t(source_stride)*source_height},source_width,source_height,source_stride,
            {affine.start_x,affine.start_y,affine.column_x,affine.column_y,affine.row_x,affine.row_y},
            0,false,false}).success)return true;
    for (uint32_t row = 0; row < height; ++row) {
        uint32_t fixed_x = static_cast<uint32_t>(affine.start_x) + 0x8000U +
            row * static_cast<uint32_t>(affine.row_x);
        uint32_t fixed_y = static_cast<uint32_t>(affine.start_y) + 0x8000U +
            row * static_cast<uint32_t>(affine.row_y);
        for (uint32_t column = 0; column < width; ++column) {
            const int32_t x = static_cast<int32_t>(fixed_x) >> 16;
            const int32_t y = static_cast<int32_t>(fixed_y) >> 16;
            auto* pixel = destination + static_cast<size_t>(row) * destination_stride +
                static_cast<size_t>(column) * 4U;
            if (x >= 0 && y >= 0 && static_cast<uint32_t>(x) < source_width &&
                static_cast<uint32_t>(y) < source_height)
                std::copy_n(source + static_cast<size_t>(y) * source_stride +
                            static_cast<size_t>(x) * 4U, 4U, pixel);
            else std::fill_n(pixel, 4U, uint8_t{0});
            fixed_x += static_cast<uint32_t>(affine.column_x);
            fixed_y += static_cast<uint32_t>(affine.column_y);
        }
    }
    return true;
}

bool sample_background_transform(uint8_t* destination,
                                 uint32_t destination_stride, uint32_t width,
                                 uint32_t height, const uint8_t* source,
                                 uint32_t source_stride, uint32_t source_width,
                                 uint32_t source_height,
                                 const BackgroundAffine& affine) {
    if (destination == nullptr || source == nullptr || width == 0 ||
        height == 0 || source_width == 0 || source_height == 0)
        return false;
    if (destination_stride < width * 4U || source_stride < source_width * 4U)
        return false;
    if(compose_host_fast_affine({{destination,size_t(destination_stride)*height},width,height,destination_stride,
            {source,size_t(source_stride)*source_height},source_width,source_height,source_stride,
            {affine.start_x,affine.start_y,affine.column_x,affine.column_y,affine.row_x,affine.row_y},
            0,true,false}).success)return true;
    for (uint32_t row = 0; row < height; ++row) {
        for (uint32_t column = 0; column < width; ++column) {
            const uint32_t fixed_x =
                static_cast<uint32_t>(affine.start_x) +
                column * static_cast<uint32_t>(affine.column_x) +
                row * static_cast<uint32_t>(affine.row_x);
            const uint32_t fixed_y =
                static_cast<uint32_t>(affine.start_y) +
                column * static_cast<uint32_t>(affine.column_y) +
                row * static_cast<uint32_t>(affine.row_y);
            const int32_t source_x = static_cast<int32_t>(fixed_x) >> 16;
            const int32_t source_y = static_cast<int32_t>(fixed_y) >> 16;
            const int32_t fraction_x =
                (static_cast<int32_t>(fixed_x) >> 12) & 0xF;
            const int32_t fraction_y =
                (static_cast<int32_t>(fixed_y) >> 12) & 0xF;
            uint8_t* pixel = destination +
                             static_cast<std::size_t>(row) *
                                 destination_stride +
                             static_cast<std::size_t>(column) * 4U;
            for (uint32_t channel = 0; channel < 4U; ++channel) {
                const int32_t top_left = source_byte(
                    source, source_stride, source_width, source_height,
                    source_x, source_y, channel);
                const int32_t top_right = source_byte(
                    source, source_stride, source_width, source_height,
                    source_x + 1, source_y, channel);
                const int32_t bottom_left = source_byte(
                    source, source_stride, source_width, source_height,
                    source_x, source_y + 1, channel);
                const int32_t bottom_right = source_byte(
                    source, source_stride, source_width, source_height,
                    source_x + 1, source_y + 1, channel);
                const int32_t top =
                    top_left + (((top_right - top_left) * fraction_x) >> 4);
                const int32_t bottom = bottom_left +
                    (((bottom_right - bottom_left) * fraction_x) >> 4);
                int32_t value = top + (((bottom - top) * fraction_y) >> 4);
                if (value < 0) {
                    value = 0;
                } else if (value > 0xFF) {
                    value = 0xFF;
                }
                pixel[channel] = static_cast<uint8_t>(value);
            }
        }
    }
    return true;
}
}  // namespace bgiplayer
