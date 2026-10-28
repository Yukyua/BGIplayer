#include "bgiplayer/gpu_pixel_kernels.hpp"
#include "bgiplayer/debug.hpp"
#include "bgiplayer/sprite_scaled_alpha.hpp"
#include "bgiplayer/sprite_crossfade.hpp"
#include "bgiplayer/sprite_rotated_alpha.hpp"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#if !defined(__APPLE__) && !defined(__ANDROID__)
namespace bgiplayer {
struct GpuPixelKernels::Impl {};
std::shared_ptr<GpuPixelStorage> GpuPixelKernels::store_pixels(std::span<const uint8_t>) { return {}; }
PixelKernelResult GpuPixelKernels::pixel_transfer(const PixelTransferPixels&) { return {}; }
PixelKernelResult GpuPixelKernels::affine(const AffinePixels&) { return {}; }
PixelKernelResult GpuPixelKernels::layer_composite(const LayerCompositePixels&) { return {}; }
PixelKernelResult GpuPixelKernels::background_mask(const BackgroundMaskPixels&) { return {}; }
GpuPixelKernels::GpuPixelKernels(bool) : impl_(std::make_unique<Impl>()) {}
GpuPixelKernels::~GpuPixelKernels() = default;
bool GpuPixelKernels::available() const { return false; }
std::string GpuPixelKernels::device_name() const { return "CPU"; }
PixelKernelResult GpuPixelKernels::scaled_alpha(const ScaledAlphaPixels& p) {
    const auto start=debug_enabled()?std::chrono::steady_clock::now():std::chrono::steady_clock::time_point{};
    const bool success=compose_scaled_alpha_bgra(p.destination,p.destination_width,p.destination_height,
        p.destination_stride,p.source,p.source_width,p.source_height,p.source_stride,
        p.x_q16,p.y_q16,p.origin_x_q16,p.origin_y_q16,p.scale_x_q16,p.scale_y_q16,p.parameter,p.filtered);
    const auto elapsed=debug_enabled()?std::chrono::steady_clock::now()-start:std::chrono::steady_clock::duration{};
    return {success,false,static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()),0,0,0};
}
PixelKernelResult GpuPixelKernels::crossfade(const CrossfadePixels& p) {
    const auto start=debug_enabled()?std::chrono::steady_clock::now():std::chrono::steady_clock::time_point{};
    const bool success=blend_original_sprite_crossfade(p.destination,p.first,p.second,p.mix,p.opacity);
    const auto elapsed=debug_enabled()?std::chrono::steady_clock::now()-start:std::chrono::steady_clock::duration{};
    return {success,false,static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()),0,0,0};
}
}
#endif

namespace bgiplayer {
namespace {
struct HostState {
    GpuPixelKernels cpu;
    std::unique_ptr<GpuPixelKernels> gpu;
    HostPixelKernelStatistics statistics;
};
HostState& state() { static thread_local HostState value; return value; }
bool choose_gpu(uint64_t pixels,uint64_t threshold) {
    const char* mode=std::getenv("BGI_GPU");
    if(mode && std::strcmp(mode,"0")==0)return false;
    return (mode && std::strcmp(mode,"1")==0) || pixels>=threshold;
}
GpuPixelKernels& backend(bool use_gpu) {
    auto& host=state();
    if(!use_gpu)return host.cpu;
    if(!host.gpu)host.gpu=std::make_unique<GpuPixelKernels>(true);
    return *host.gpu;
}
PixelKernelResult record(const char* kernel,PixelKernelResult result,uint64_t pixels) {
    if(!debug_enabled())return result;
    auto& stats=state().statistics;
    ++stats.calls;
    if(result.used_gpu)++stats.gpu_dispatches;else ++stats.cpu_calls;
    if(result.approximate)++stats.approximate_dispatches;
    if(debug_trace(DebugTrace::Gpu) && (stats.calls<=16 || stats.calls%256==0 ||
            (result.used_gpu && stats.gpu_dispatches<=16)))
        std::cout<<"GPU_KERNEL kind="<<kernel<<" pixels="<<pixels<<" used_gpu="<<result.used_gpu
                 <<" success="<<result.success<<" total_ns="<<result.total_ns
                 <<" approximate="<<result.approximate
                 <<" upload_ns="<<result.upload_ns<<" wait_ns="<<result.wait_ns
                 <<" download_ns="<<result.download_ns<<" calls="<<stats.calls
                 <<" gpu_dispatches="<<stats.gpu_dispatches<<" cpu_calls="<<stats.cpu_calls<<'\n';
    return result;
}
}
PixelKernelResult compose_host_scaled_alpha(const ScaledAlphaPixels& p) {
    if(fast_rendering_enabled() && p.scale_x_q16>32768U && p.scale_y_q16>32768U &&
            p.source_stride<=32764U && p.destination_stride<=2147483647U) {
        const auto matrix=inverse_sprite_affine_q16(p.x_q16,p.y_q16,p.origin_x_q16,p.origin_y_q16,
            0,p.scale_x_q16,p.scale_y_q16);
        if(matrix) {
            auto result=compose_host_fast_affine({p.destination,p.destination_width,p.destination_height,
                p.destination_stride,p.source,p.source_width,p.source_height,p.source_stride,
                *matrix,p.parameter,p.filtered,true});
            if(result.success)return result;
        }
    }
    const uint64_t pixels=uint64_t(p.destination_width)*p.destination_height;
    return record("scaled_alpha",backend(choose_gpu(pixels,p.filtered?512U*512U:1920U*1080U)).scaled_alpha(p),pixels);
}
PixelKernelResult compose_host_sprite_crossfade(const CrossfadePixels& p) {
    const uint64_t pixels=uint64_t(std::min(p.first.width,p.second.width))*std::min(p.first.height,p.second.height);
    return record("crossfade",backend(choose_gpu(pixels,512U*512U)).crossfade(p),pixels);
}
HostPixelKernelStatistics host_pixel_kernel_statistics() { return state().statistics; }
PixelKernelResult compose_host_fast_affine(const AffinePixels& p) {
    if(!fast_rendering_enabled() || !choose_gpu(uint64_t(p.destination_width)*p.destination_height,
            p.filtered?256U*256U:512U*512U))return {};
    auto result=backend(true).affine(p);
    return result.success?record("affine_fast",result,uint64_t(p.destination_width)*p.destination_height):result;
}
PixelKernelResult compose_host_fast_background_mask(const BackgroundMaskPixels& p) {
    if(p.left>=p.right || p.top>=p.bottom)return {};
    const uint64_t pixels=uint64_t(p.right-p.left)*(p.bottom-p.top);
    if(!fast_rendering_enabled() || !choose_gpu(pixels,1024U*768U))return {};
    auto result=backend(true).background_mask(p);
    return result.success?record("background_mask",result,pixels):result;
}

PixelKernelResult compose_host_layer_composite(const LayerCompositePixels& p) {
    const uint64_t pixels=uint64_t(p.destination.width)*p.destination.height;
    if(!choose_gpu(pixels,p.source_storage?0U:512U*512U))return {};
    auto result=backend(true).layer_composite(p);
    return result.success?record("layer_composite",result,pixels):result;
}

bool valid_pixel_transfer(const PixelTransferPixels& p) {
    const auto valid=[](const GlyphBitmapView& v,size_t size) {
        return v.width && v.height && v.bytes_per_pixel==4 && (v.format==1 || v.format==2) &&
            size<=UINT32_MAX && v.stride>=uint64_t(v.width)*4 &&
            uint64_t(v.byte_offset)+uint64_t(v.height-1)*v.stride+uint64_t(v.width)*4<=size;
    };
    if(!valid(p.destination,p.destination.pixels.size()) || p.parameter>256)return false;
    if(p.operation==PixelTransferOperation::Clear)return true;
    if(p.operation==PixelTransferOperation::ColorFilter)return p.destination.format==1;
    if(!valid(p.source,p.source_storage?p.source_storage->size():p.source.pixels.size()) || p.source.width!=p.destination.width || p.source.height!=p.destination.height)return false;
    const uintptr_t a=reinterpret_cast<uintptr_t>(p.destination.pixels.data()),
                    b=reinterpret_cast<uintptr_t>(p.source.pixels.data());
    if(!p.source_storage && (a<=b?b-a<p.destination.pixels.size():a-b<p.source.pixels.size()))return false;
    if(p.source_storage && !p.source.pixels.empty())return false;
    switch(p.operation) {
        case PixelTransferOperation::Copy:return true;
        case PixelTransferOperation::FadeSource:
        case PixelTransferOperation::BlendOpaque:return p.destination.format==1 && p.source.format==1;
        case PixelTransferOperation::BlendAlpha:return p.destination.format==1 && p.source.format==2 && p.parameter>0 && p.parameter<256;
        default:return false;
    }
}
PixelKernelResult compose_host_pixel_transfer(const PixelTransferPixels& p) {
    const uint64_t pixels=uint64_t(p.destination.width)*p.destination.height;
    if(!choose_gpu(pixels,p.source_storage?0U:512U*512U))return {};
    const auto result=backend(true).pixel_transfer(p);
    constexpr const char* names[]={"copy","source_fade","opaque_opacity","color_filter","alpha_opacity","clear"};
    return result.success?record(names[static_cast<uint32_t>(p.operation)],result,pixels):result;
}

std::shared_ptr<GpuPixelStorage> store_host_pixel_storage(std::span<const uint8_t> pixels) {
    if(retain_cpu_pixel_copies() || pixels.empty())return {};
    return backend(true).store_pixels(pixels);
}

}
