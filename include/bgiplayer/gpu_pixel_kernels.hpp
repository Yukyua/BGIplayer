#pragma once
#include "bgiplayer/glyph.hpp"
#include "bgiplayer/render_policy.hpp"
#include "bgiplayer/pixel_storage.hpp"
#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>

namespace bgiplayer {
struct ScaledAlphaPixels {
    std::span<uint8_t> destination;
    uint32_t destination_width, destination_height, destination_stride;
    std::span<const uint8_t> source;
    uint32_t source_width, source_height, source_stride;
    int32_t x_q16, y_q16, origin_x_q16, origin_y_q16;
    uint32_t scale_x_q16, scale_y_q16, parameter;
    bool filtered;
};
struct CrossfadePixels {
    GlyphBitmapView destination, first, second;
    int32_t mix;
    uint32_t opacity;
};
struct AffinePixels {
    std::span<uint8_t> destination;
    uint32_t destination_width,destination_height,destination_stride;
    std::span<const uint8_t> source;
    uint32_t source_width,source_height,source_stride;
    // Q16 source start, column step, row step; original integer geometry.
    std::array<int32_t,6> matrix;
    uint32_t parameter=0;
    bool filtered=false;
    bool blend_alpha=true;
};
// Pre-clipped original mask blend. Factor -1 copies BGRA, zero skips;
// positive factors blend RGB with an arithmetic shift of7 or12.
struct BackgroundMaskPixels {
    GlyphBitmapView destination, source, mask;
    uint32_t left, top, right, bottom, source_left, source_top, mask_left, mask_top;
    std::array<int32_t,256> factors;
    uint32_t shift;
};
struct LayerCompositePixels {
    GlyphBitmapView destination, source; // equally sized, pre-clipped views
    std::span<const uint8_t> reciprocal_corrections;
    std::shared_ptr<GpuPixelStorage> source_storage{};
};
// Exact pre-clipped host transfers used by original bitmap/display callbacks.
// Copy retains the format-1/2 conversion rules; opacity parameters are0..256.
enum class PixelTransferOperation : uint32_t {
    Copy, FadeSource, BlendOpaque, ColorFilter, BlendAlpha, Clear
};
struct PixelTransferPixels {
    GlyphBitmapView destination, source;
    PixelTransferOperation operation = PixelTransferOperation::Copy;
    uint32_t parameter = 0, color = 0;
    std::shared_ptr<GpuPixelStorage> source_storage{};
};
bool valid_pixel_transfer(const PixelTransferPixels& pixels);
struct PixelKernelResult {
    bool success = false;
    bool used_gpu = false;
    // Diagnostic timings are zero unless runtime debug/profiling is enabled.
    uint64_t total_ns = 0, upload_ns = 0, wait_ns = 0, download_ns = 0;
    bool approximate = false;
};
// Host-only accelerator: no engine/snapshot state. Disabled by default. Own on
// one thread; the synchronous call completes all writes before returning.
class GpuPixelKernels {
public:
    explicit GpuPixelKernels(bool enabled = false);
    ~GpuPixelKernels();
    GpuPixelKernels(const GpuPixelKernels&) = delete;
    GpuPixelKernels& operator=(const GpuPixelKernels&) = delete;
    [[nodiscard]] bool available() const;
    [[nodiscard]] std::string device_name() const;
    PixelKernelResult scaled_alpha(const ScaledAlphaPixels& pixels);
    PixelKernelResult crossfade(const CrossfadePixels& pixels);
    PixelKernelResult affine(const AffinePixels& pixels);
    PixelKernelResult layer_composite(const LayerCompositePixels& pixels);
    PixelKernelResult background_mask(const BackgroundMaskPixels& pixels);
    PixelKernelResult pixel_transfer(const PixelTransferPixels& pixels);
    std::shared_ptr<GpuPixelStorage> store_pixels(std::span<const uint8_t> pixels);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
// Auto chooses GPU only above measured conservative thresholds. BGI_GPU=0
// forces CPU; BGI_GPU=1 requests GPU even for small inputs. Failures fall back.
PixelKernelResult compose_host_scaled_alpha(const ScaledAlphaPixels& pixels);
PixelKernelResult compose_host_sprite_crossfade(const CrossfadePixels& pixels);
// Returns unsuccessful without modifying the target when exact mode, small
// inputs, unavailable hardware or an unsupported pixel view selects CPU fallback.
PixelKernelResult compose_host_fast_affine(const AffinePixels& pixels);
// Fast-policy dispatch, but integer mask results retain exact CPU byte values.
PixelKernelResult compose_host_fast_background_mask(const BackgroundMaskPixels& pixels);
PixelKernelResult compose_host_layer_composite(const LayerCompositePixels& pixels);
PixelKernelResult compose_host_pixel_transfer(const PixelTransferPixels& pixels);
struct HostPixelKernelStatistics { uint64_t calls=0, gpu_dispatches=0, cpu_calls=0, approximate_dispatches=0; };
HostPixelKernelStatistics host_pixel_kernel_statistics();
}
