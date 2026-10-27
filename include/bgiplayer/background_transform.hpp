#pragma once

#include <cstdint>
#include <optional>
#include <utility>

namespace bgiplayer {

// 00419AE0 input. All values are Q16 except the angle, which is Q16 degrees
// (0x1680000 is one full turn).
struct BackgroundTransform {
    int32_t origin_x = 0;
    int32_t origin_y = 0;
    int32_t angle = 0;
    int32_t scale_x = 0x10000;
    int32_t scale_y = 0x10000;
};

// 00419AE0 output: destination (0,0) maps to source (start_x,start_y) and the
// destination column/row vectors add (column_*, row_*).
struct BackgroundAffine {
    int32_t start_x = 0;
    int32_t start_y = 0;
    int32_t column_x = 0;
    int32_t column_y = 0;
    int32_t row_x = 0;
    int32_t row_y = 0;
};

// 004199D0: integer/unit transform AND the shifted source rectangle fully
// contains the shifted destination. Returns the source crop's integer origin.
std::optional<std::pair<int32_t,int32_t>> classify_background_exact_copy(
    const BackgroundTransform& transform, int32_t destination_x,
    int32_t destination_y, uint32_t destination_width,
    uint32_t destination_height, uint32_t source_width, uint32_t source_height);

// Direct translation of 00419AE0 for the positive-scale case. Zero or
// negative scales are an explicit unsupported arm.
std::optional<BackgroundAffine> evaluate_background_affine(
    const BackgroundTransform& transform, int32_t destination_x,
    int32_t destination_y);

// PE64 140052700 preserves its own trigonometric operation order.
std::optional<BackgroundAffine> evaluate_pe64_background_affine(
    const BackgroundTransform& transform, int32_t destination_x,
    int32_t destination_y);

// PE64 14004FE80: add half a Q16 pixel, copy all four bytes of the
// nearest source pixel, or write zero when outside the source rectangle.
bool sample_background_transform_nearest(
    uint8_t* destination, uint32_t destination_stride, uint32_t width,
    uint32_t height, const uint8_t* source, uint32_t source_stride,
    uint32_t source_width, uint32_t source_height,
    const BackgroundAffine& affine);

// Direct translation of 0041BEC0's four-neighbor 1/16 bilinear kernel. Pixels
// are four opaque bytes in source/target storage order. Every destination
// pixel is written; out-of-range source neighbors contribute zero bytes.
bool sample_background_transform(
    uint8_t* destination, uint32_t destination_stride, uint32_t width,
    uint32_t height, const uint8_t* source, uint32_t source_stride,
    uint32_t source_width, uint32_t source_height,
    const BackgroundAffine& affine);

}  // namespace bgiplayer
