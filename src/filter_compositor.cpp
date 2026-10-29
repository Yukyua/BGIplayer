#include "bgiplayer/vm.hpp"
#include "bgiplayer/gpu_pixel_kernels.hpp"
#include "bgiplayer/gpu_exact_pixel_ops.hpp"
#include "bgiplayer/filter_color.hpp"
#include "bgiplayer/filter_mask.hpp"
#include "bgiplayer/filter_rgb555.hpp"
#include <cstdlib>
#include <iostream>

namespace bgiplayer {
void VM::refresh_filter_display_geometry(uint32_t object) {
    if (!display_object_states_.contains(object)) register_display_object(object,0,object);
    auto& state = display_object_states_.at(object);
    state.evaluate_priority = true;
    const auto [width,height] = current_mode_dimensions();
    update_display_object_geometry(object,0,0,width,height,object_draw_visible(object));
    display_dirty_ = true;
}

bool VM::compose_filter_at(uint32_t destination, uint32_t object,
                           uint32_t mask_left, uint32_t mask_top) {
    const auto fail = [&](std::string_view reason, uint32_t branch = 0) {
        return fail_display_draw(destination, object, "filter", reason, branch);
    };
    const auto filter = filter_states_.find(object);
    auto target = graphics_surfaces_.find(destination);
    if (filter == filter_states_.end()) return fail("filter state is missing");
    if (target == graphics_surfaces_.end()) return fail("filter destination bitmap is missing");
    if (!materialize_graphics_pixels(target->second)) return fail("filter destination pixels could not be materialized");
    // 00425DD0 raw mode0 / field148=0 ->00411B30. The bitmap-mask
    // and other color operations remain separate original kernels.
    const auto& f = filter->second;
    if (debug_trace(DebugTrace::Display)) {
        static std::set<std::array<uint32_t,5>> recorded;
        const std::array<uint32_t,5> key{object,f.field_148,f.field_14c,object_effect_level(object),f.active};
        if (recorded.insert(key).second)
            std::cout << "FILTER_RENDER object=" << object << " mode=" << f.field_148
                      << " color=" << f.field_14c << " effect=" << object_effect_level(object)
                      << " bitmap_mode=" << f.active << '\n';
    }
    // Original00425E6D/14005C41F return without drawing for every selector
    // outside0..3. This is an admitted callback no-op, not a missing kernel.
    if (!f.active && f.field_148 > 3) return true;
    const uint32_t threshold=object_visibility_threshold(object), effect=object_effect_level(object);
    const uint32_t gate=object_raw_field(object,0xBCU);
    uint32_t weight=effect;
    switch (object_raw_field(object,0xB0U)) { //004208E0
        case 1: case 0x20: case 0x21: case 0x22: case 0x23: case 0x24:
            weight=0x100U-(((0x100U-threshold)*(0x100U-effect)*gate)>>16U); break;
        case 2: case 3: case 4: case 0xC0: case 0xC1:
            weight=((0x100U-threshold)*effect*gate)>>16U; break;
        default: break;
    }
    if (weight>256) return fail("filter effective strength exceeds 256");
    auto& out=target->second;
    const auto exact_filter = [&](ExactPixelOperation operation, GraphicsSurface& source, uint32_t source_offset=0) {
        if(!exact_pixels_worthwhile(out.width,out.height,bool(out.pixels.gpu_storage())))return false;
        const auto read_view=[](const PixelBuffer& bytes) {
            if(bytes.gpu_storage())return std::span<uint8_t>{};
            const auto cpu=bytes.upload_bytes();
            return std::span<uint8_t>(const_cast<uint8_t*>(cpu.data()),cpu.size());
        };
        ExactPixelPixels pixels{
            {read_view(out.pixels),0,out.stride,out.width,out.height,out.format,out.bytes_per_pixel},
            {read_view(source.pixels),source_offset,source.stride,out.width,out.height,source.format,source.bytes_per_pixel},
            {},operation};
        pixels.destination_storage=out.pixels.gpu_storage();
        pixels.source_storage=source.pixels.gpu_storage();
        pixels.parameters[0]=f.field_14c;
        pixels.parameters[1]=weight;
        pixels.parameters[2]=f.field_154;
        auto result=compose_host_resident_exact_pixels(pixels);
        if(!result)return false;
        if(retain_cpu_pixel_copies()) {
            std::vector<uint8_t> bytes(result->size());
            if(!result->read(bytes))return false;
            out.pixels=std::move(bytes);
        } else out.pixels.replace_gpu_storage(std::move(result));
        return true;
    };
    if (out.format==0 && out.bytes_per_pixel==2) {
        // Only mode0 has an original16-bit pixel arm. Other color selectors
        // and active bitmap masks return without drawing in both profiles.
        if (f.active || f.field_148!=0) return true;
        if (!exact_filter(ExactPixelOperation::FilterRgb555,out) &&
            !filter_rgb555_color(out.pixels,out.stride,out.pixels,out.stride,
                out.width,out.height,f.field_14c,weight))
            return fail("RGB555 color filter rejected its pixel views");
        // The host has no verified format0 presentation conversion here.
        // Keep raw native pixels authoritative instead of inventing RGBA.
        out.rgba.clear();
        return true;
    }
    if ((out.format!=1 && out.format!=2) || out.bytes_per_pixel!=4)
        return fail("filter destination pixel format is not implemented");
    if (f.active) {
        auto mask=graphics_surfaces_.find(f.bitmap);
        // 00425E09/00425E27 return without drawing when the descriptor is
        // absent or its original allocation identity no longer matches.
        if (mask==graphics_surfaces_.end() || graphics_bitmap_token(f.bitmap)!=f.bitmap_token)
            return true;
        // The original dispatcher00412130 accepts format1 only. Format2
        // completes without touching pixels or changing object state.
        if (out.format==2) return true;
        auto& source=mask->second;
        if (source.format!=3 || source.bytes_per_pixel!=1)
            return fail("filter mask pixel format is invalid");
        if (!materialize_graphics_pixels(source)) return fail("filter mask pixels could not be materialized");
        if (uint64_t(mask_left)+out.width>source.width || uint64_t(mask_top)+out.height>source.height)
            return fail("filter mask clip exceeds its bitmap");
        const uint64_t offset=uint64_t(mask_top)*source.stride+mask_left;
        if (offset>source.pixels.size() || (!(offset<=UINT32_MAX && exact_filter(ExactPixelOperation::FilterMask,source,uint32_t(offset))) &&
            !filter_bitmap_mask(out.pixels,out.stride,
                std::span<const uint8_t>(source.pixels).subspan(size_t(offset)),source.stride,
                out.width,out.height,f.field_14c,f.field_154,weight)))
            return fail("bitmap-mask filter rejected its pixel views");
        refresh_bgra_mirror(out);
        return true;
    }
    if (f.field_148 == 1) {
        if (!exact_filter(ExactPixelOperation::FilterAddSub,out) &&
            !filter_addsub_color(out.pixels,out.stride,out.pixels,out.stride,
                out.width,out.height,out.format,f.field_14c,weight))
            return fail("add/subtract color filter rejected its pixel views",1);
        refresh_bgra_mirror(out);
        return true;
    }
    if (f.field_148 == 3) {
        if (!exact_filter(ExactPixelOperation::FilterXor,out) &&
            !filter_xor_color(out.pixels,out.stride,out.pixels,out.stride,
                out.width,out.height,out.format,f.field_14c,weight))
            return fail("XOR color filter rejected its pixel views",3);
        refresh_bgra_mirror(out);
        return true;
    }
    if (f.field_148 == 2) {
        if (!exact_filter(ExactPixelOperation::FilterLuminance,out) &&
            !filter_luminance_color(out.pixels,out.stride,out.pixels,out.stride,
                out.width,out.height,out.format,f.field_14c,weight))
            return fail("luminance color filter rejected its pixel views",2);
        refresh_bgra_mirror(out);
        return true;
    }
    if (f.field_14c == 0 && out.format == 2U) {
        if (!exact_filter(ExactPixelOperation::FilterBlackAlpha,out) &&
            !filter_black_alpha(out.pixels,out.stride,out.pixels,out.stride,
                out.width,out.height,weight))
            return fail("black source-alpha filter rejected its pixel views");
        refresh_bgra_mirror(out);
        return true;
    }
    if(compose_host_pixel_transfer({{out.pixels,0,out.stride,out.width,out.height,out.format,4},
            {},PixelTransferOperation::ColorFilter,weight,f.field_14c}).success) {
        refresh_bgra_mirror(out);return true;
    }
    // 00411C80 preserves alpha; the color0 specialization0040EAF0
    // writes RGB only and zeroes alpha whenever weight is nonzero.
    for (uint32_t y=0;y<out.height;++y) for (uint32_t x=0;x<out.width;++x) {
        const std::size_t p=static_cast<std::size_t>(y)*out.stride+x*4U;
        for (uint32_t channel=0;channel<3;++channel) {
            const uint32_t color=(f.field_14c>>(channel*8U))&255U;
            out.pixels[p+channel]=static_cast<uint8_t>((out.pixels[p+channel]*(256U-weight)+color*weight)>>8U);
        }
        if (f.field_14c == 0 && weight != 0) out.pixels[p+3U] = 0;
    }
    refresh_bgra_mirror(out);
    return true;
}
} // namespace bgiplayer
