#pragma once
#include "bgiplayer/pixel_storage.hpp"
#include <span>
namespace bgiplayer {
// Each immutable source is tightly packed (row_bytes * rows). Destination
// rows begin at offset + y*stride; later patches overwrite earlier patches.
struct GpuRawPatch {
    std::shared_ptr<GpuPixelStorage> pixels;
    size_t offset=0,stride=0,row_bytes=0,rows=0;
};
bool valid_gpu_raw_patches(const std::shared_ptr<GpuPixelStorage>& base,std::span<const GpuRawPatch> patches);
std::shared_ptr<GpuPixelStorage> compose_host_raw_patches(std::shared_ptr<GpuPixelStorage> base,
    std::span<const GpuRawPatch> patches);
struct GpuRawPatchStatistics {uint64_t batches=0,dispatches=0;};
GpuRawPatchStatistics gpu_raw_patch_statistics();
namespace raw_patch_detail {
std::shared_ptr<GpuPixelStorage> execute(const std::shared_ptr<GpuPixelStorage>& base,
    std::span<const GpuRawPatch> patches);
}
}
