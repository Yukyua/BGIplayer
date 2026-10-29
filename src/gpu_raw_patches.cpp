#include "bgiplayer/gpu_raw_patches.hpp"
#include <cstdlib>
#include <cstring>
namespace bgiplayer {
namespace {thread_local GpuRawPatchStatistics statistics;}
GpuRawPatchStatistics gpu_raw_patch_statistics(){return statistics;}
bool valid_gpu_raw_patches(const std::shared_ptr<GpuPixelStorage>& base,std::span<const GpuRawPatch> patches) {
    if(!base || !base->size() || base->size()>UINT32_MAX || patches.empty() || patches.size()>256)return false;
    for(const auto& p:patches) {
        if(!p.pixels || !p.pixels->size() || p.pixels->size()>UINT32_MAX || !p.row_bytes || !p.rows ||
                p.offset>UINT32_MAX || p.stride>UINT32_MAX || p.row_bytes>UINT32_MAX || p.rows>UINT32_MAX ||
                p.stride<p.row_bytes || p.offset>base->size() ||
                p.rows>p.pixels->size()/p.row_bytes ||
                p.rows-1>(base->size()-p.offset)/p.stride ||
                p.row_bytes>base->size()-p.offset-(p.rows-1)*p.stride)return false;
    }
    return true;
}
std::shared_ptr<GpuPixelStorage> compose_host_raw_patches(std::shared_ptr<GpuPixelStorage> base,
        std::span<const GpuRawPatch> patches) {
    const char* mode=std::getenv("BGI_GPU");
    if((mode && std::strcmp(mode,"0")==0) || !valid_gpu_raw_patches(base,patches) ||
            (!(mode && std::strcmp(mode,"1")==0) && base->size()<65536U))return {};
    auto result=raw_patch_detail::execute(base,patches);
    if(result){++statistics.batches;statistics.dispatches+=patches.size();}
    return result;
}
#if !defined(__APPLE__) && !defined(__ANDROID__)
namespace raw_patch_detail {
std::shared_ptr<GpuPixelStorage> execute(const std::shared_ptr<GpuPixelStorage>&,std::span<const GpuRawPatch>){return {};}
}
#endif
}
