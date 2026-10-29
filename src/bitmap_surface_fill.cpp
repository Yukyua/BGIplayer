#include "bgiplayer/vm.hpp"
#include "bgiplayer/gpu_pixel_kernels.hpp"

namespace bgiplayer {
// Original 00485C10 -> 00408E50 -> 0040B3D0; format kernels
// 0040B270 (clear), 0040B460 (packed), 0040B4E0 (BGR/A), 0040B610 (index).
void VM::set_surface_command(uint32_t type, uint32_t value) {
    graphics_surface_commands_[type] = value;
    const auto surface = graphics_surfaces_.find(type);
    if (surface == graphics_surfaces_.end()) {
        return;
    }
    GraphicsSurface& target = surface->second;
    const std::size_t size =
        static_cast<std::size_t>(target.stride) * target.height;
    if(target.format==1U || target.format==2U) {
        if(auto native=store_host_filled_pixels(size,target.format==1U?value&0xFFFFFFU:value)) {
            if(auto mirror=bgra_presentation_storage(native,static_cast<size_t>(target.width)*target.height,target.format==1U)) {
                target.pixels.replace_gpu_storage(std::move(native));
                target.rgba.replace_gpu_storage(std::move(mirror));
                display_dirty_=true;return;
            }
        }
    }
    target.pixels.assign(size, 0);
    // Resolve the mutable buffer once after full replacement. Per-component
    // PixelBuffer access repeats residency checks and blocks vectorization.
    auto* pixels = target.pixels.data();
    if (value != 0) {
        if (target.format == 0U) {
            const uint16_t packed = static_cast<uint16_t>(
                ((value >> 9U) & 0x7C00U) |
                ((value >> 6U) & 0x03E0U) |
                ((value >> 3U) & 0x001FU));
            for (std::size_t offset = 0; offset + 1U < size;
                 offset += 2U) {
                pixels[offset] =
                    static_cast<uint8_t>(packed);
                pixels[offset + 1U] =
                    static_cast<uint8_t>(packed >> 8U);
            }
        } else if (target.format == 1U) {
            const uint32_t pixel = value & 0x00FFFFFFU;
            for (std::size_t offset = 0; offset + 3U < size;
                 offset += 4U) {
                pixels[offset] =
                    static_cast<uint8_t>(pixel);
                pixels[offset + 1U] =
                    static_cast<uint8_t>(pixel >> 8U);
                pixels[offset + 2U] =
                    static_cast<uint8_t>(pixel >> 16U);
                pixels[offset + 3U] = 0;
            }
        } else if (target.format == 2U) {
            for (std::size_t offset = 0; offset + 3U < size;
                 offset += 4U) {
                pixels[offset] =
                    static_cast<uint8_t>(value);
                pixels[offset + 1U] =
                    static_cast<uint8_t>(value >> 8U);
                pixels[offset + 2U] =
                    static_cast<uint8_t>(value >> 16U);
                pixels[offset + 3U] =
                    static_cast<uint8_t>(value >> 24U);
            }
        } else if (target.format == 3U) {
            std::fill_n(pixels, size, static_cast<uint8_t>(value));
        }
    }
    const std::size_t pixel_count =
        static_cast<std::size_t>(target.width) * target.height;
    target.rgba.assign(pixel_count * 4U, 0);
    auto* rgba = target.rgba.data();
    if (target.format == 1U || target.format == 2U) {
        for (std::size_t pixel = 0; pixel < pixel_count; ++pixel) {
            const std::size_t source = pixel * 4U;
            rgba[source] = pixels[source + 2U];
            rgba[source + 1U] =
                pixels[source + 1U];
            rgba[source + 2U] =
                pixels[source];
            rgba[source + 3U] =
                target.format == 1U
                    ? 0xFFU
                    : pixels[source + 3U];
        }
    }
    display_dirty_ = true;
}
} // namespace bgiplayer
