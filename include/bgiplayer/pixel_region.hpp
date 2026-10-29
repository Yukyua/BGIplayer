#pragma once
#include "bgiplayer/pixel_storage.hpp"
namespace bgiplayer {
// Packed immutable byte view of a rectangle; preserves every channel.
std::shared_ptr<GpuPixelStorage> crop_host_pixel_storage(
    std::shared_ptr<GpuPixelStorage> source,size_t offset,size_t stride,
    size_t row_bytes,size_t rows);
}
