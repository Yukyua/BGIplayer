#pragma once
#include "bgiplayer/gpu_pixel_kernels.hpp"
#include <array>
#include <mutex>
namespace bgiplayer {
// Borrowed native context, scoped by the existing host GPU device owner.
struct NativePixelContext {
    const void* display=nullptr;const void* context=nullptr;const void* surface=nullptr;
    std::recursive_mutex* mutex=nullptr;
    std::shared_ptr<void> lifetime;
};
NativePixelContext host_native_pixel_context();

// Exact byte kernels. The CPU reference remains authoritative on rejection.
enum class ExactPixelOperation : uint32_t {
    FilterRgb555=1, FilterMask=2, FilterAddSub=3, FilterLuminance=4,
    FilterXor=5, FilterBlackAlpha=6, RawCopy=7, Fill=8,
    SpriteReduction=16, SpritePairMix=17, SpriteTerminal=18, IconRotate=19,
    VmBlit=32, Flip=33, Invert=34, ReplaceColor=35, Grayscale=36,
    ScaledKernel=37, HalfDownsample=38, ComposeAlpha=39, Expand=40,
    Resize=41, MaskedBlend=42, VmClearMirror=43
};
struct ExactPixelPixels {
    GlyphBitmapView destination{},source{},second{};
    ExactPixelOperation operation=ExactPixelOperation::RawCopy;
    std::array<uint32_t,512> parameters{};
    std::shared_ptr<GpuPixelStorage> destination_storage{},source_storage{},second_storage{};
    size_t destination_bytes=0; // allocation-only full Fill; no CPU initialization

};
class PixelBuffer;
bool compose_host_exact_pixel_buffers(ExactPixelPixels request,PixelBuffer& destination,
    const PixelBuffer* source=nullptr,const PixelBuffer* second=nullptr,bool cpu_result=false);
struct ExactPixelStatistics { uint64_t dispatches=0; std::array<uint64_t,64> operations{}; };
ExactPixelStatistics exact_pixel_statistics();
bool exact_pixels_worthwhile(uint32_t width,uint32_t height,bool resident=false);
bool valid_exact_pixels(const ExactPixelPixels& pixels);
PixelKernelResult compose_host_exact_pixels(const ExactPixelPixels& pixels);
// Publishes a GPU owner only after successful command encoding. Existing owners
// and source pixels remain immutable, including an in-place point operation.
std::shared_ptr<GpuPixelStorage> compose_host_resident_exact_pixels(const ExactPixelPixels& pixels);

namespace exact_pixel_detail {
std::array<uint32_t,536> arguments(const ExactPixelPixels& pixels);
std::string shader_body();
PixelKernelResult execute(const ExactPixelPixels& pixels);
std::shared_ptr<GpuPixelStorage> execute_resident(const ExactPixelPixels& pixels);
}
}
