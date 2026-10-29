#pragma once
#include "bgiplayer/pixel_storage.hpp"
#include <span>
#include <string_view>

namespace bgiplayer {
// One bounded host cache per thread, shared by all immutable pixel operations.
// Callers validate their descriptors and policy before consulting the cache.
// Tags identify an operation/version; arguments include every output dependency.
bool gpu_pixel_cache_enabled();
std::shared_ptr<GpuPixelStorage> find_gpu_pixel_cache(std::string_view tag,
    std::span<const std::shared_ptr<GpuPixelStorage>> inputs,std::span<const uint64_t> arguments);
void store_gpu_pixel_cache(std::string_view tag,
    std::span<const std::shared_ptr<GpuPixelStorage>> inputs,std::span<const uint64_t> arguments,
    std::shared_ptr<GpuPixelStorage> result);
void clear_gpu_pixel_cache();
struct GpuPixelCacheStatistics { size_t entries=0,bytes=0;uint64_t hits=0,misses=0; };
GpuPixelCacheStatistics gpu_pixel_cache_statistics();
}
