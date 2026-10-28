#include "bgiplayer/vm.hpp"
#include "bgiplayer/gpu_pixel_kernels.hpp"
#include "bgiplayer/background_mask.hpp"

#include <cstdlib>
#include <iostream>
#include <set>
#include <sstream>

namespace bgiplayer {
uint32_t VM::configure_sprite_mask(uint32_t effect, const SpriteMaskConfig& in) {
    if (debug_trace(DebugTrace::Display)) {
        std::cout << "BACK_F_CONFIG primary=" << in.primary_bitmap << " secondary=" << in.secondary_bitmap
                  << " mask=" << in.mask_bitmap << " effect=" << effect
                  << " position=" << in.field1 << ',' << in.field2
                  << " secondary_position=" << in.field4 << ',' << in.field5
                  << " mask_parameter=" << in.field8 << '\n';
    }
    // 00444A40 unconditionally selects CDspObjBackF before either setter.
    const bool replaced = background_type_ != 4U;
    select_background_class(4);
    if (replaced) {
        sprite_mask_configs_.erase(0);
        background_position_ = {0, 0};
        object_raw_fields_[{0, 0xB0U}] = 1;
        set_raw_field_recursive(0, 0xB4U, 0);
    }
    const auto primary = graphics_surfaces_.find(in.primary_bitmap);
    if (primary == graphics_surfaces_.end()) return 1;
    const bool optional = in.secondary_bitmap == 0x7000U || in.secondary_bitmap == 0x7001U ||
        in.secondary_bitmap == 0x7FFFU || in.secondary_bitmap == 0xFFFFFFFFU;
    if (!optional && graphics_surfaces_.count(in.secondary_bitmap) == 0) return 2;
    // 004225F0 commits these six fields before004226E0 validates the mask.
    SpriteMaskConfig committed = in;
    if (const auto previous = sprite_mask_configs_.find(0); previous != sprite_mask_configs_.end()) {
        committed.mask_bitmap = previous->second.mask_bitmap;
        committed.field8 = previous->second.field8;
    } else committed.mask_bitmap = 0xFFFFFFFFU;
    sprite_mask_configs_[0] = committed;
    background_bitmaps_[0] = static_cast<int32_t>(in.primary_bitmap);
    background_bitmaps_[1] = static_cast<int32_t>(in.secondary_bitmap);
    background_bitmap_tokens_[0] = graphics_bitmap_token(in.primary_bitmap);
    if (!optional) background_bitmap_tokens_[1] = graphics_bitmap_token(in.secondary_bitmap);
    if (in.mask_bitmap != 0xFFFFFFFFU) {
        const auto mask = graphics_surfaces_.find(in.mask_bitmap);
        if (mask == graphics_surfaces_.end()) return 3;
        if (mask->second.format != 3U) return 4;
        background_bitmap_tokens_[2] = graphics_bitmap_token(in.mask_bitmap);
    }
    sprite_mask_configs_[0] = in;
    background_bitmaps_[2] = static_cast<int32_t>(in.mask_bitmap);
    background_effect_level_ = effect;
    set_raw_field_recursive(0, 0xB4U, effect);
    display_dirty_ = true;
    return 0;
}

bool VM::compose_background_f_at(uint32_t destination, const DisplayBitmapDraw& callback) {
    const auto fail = [&](std::string_view reason) {
        const auto c = sprite_mask_configs_.find(0);
        return fail_display_draw(destination, main_background_handle, "backf", reason,
                                 c == sprite_mask_configs_.end() ? 0U : c->second.field8);
    };
    auto target = graphics_surfaces_.find(destination);
    if (target == graphics_surfaces_.end() || !materialize_graphics_pixels(target->second))
        return fail("BackF destination bitmap is missing or cannot be materialized");
    const auto clear = [&] {
        auto& out = target->second;
        const std::size_t bytes = static_cast<std::size_t>(out.width) * out.bytes_per_pixel;
        if (out.stride < bytes || out.pixels.size() < static_cast<std::size_t>(out.stride) * out.height)
            return fail("BackF clear destination stride or capacity is invalid");
        if(compose_host_pixel_transfer({{out.pixels,0,out.stride,out.width,out.height,out.format,out.bytes_per_pixel},
                {},PixelTransferOperation::Clear}).success) {
            refresh_bgra_mirror(out);return true;
        }
        for (std::size_t y = 0; y < out.height; ++y)
            std::fill_n(out.pixels.begin() + static_cast<std::ptrdiff_t>(y * out.stride), bytes, uint8_t{0});
        refresh_bgra_mirror(out);
        return true;
    };
    const auto config = sprite_mask_configs_.find(0);
    if (config == sprite_mask_configs_.end()) return clear();
    const auto& c = config->second;
    const auto first = graphics_surfaces_.find(c.primary_bitmap);
    // 00422840 validates the allocation token; failure reaches common clear.
    if (first == graphics_surfaces_.end() ||
        graphics_bitmap_token(c.primary_bitmap) != background_bitmap_tokens_[0]) return clear();
    const uint32_t blend = object_raw_field(0, 0xB0U);
    const uint32_t effect = object_effect_level(0);
    const uint32_t threshold = object_visibility_threshold(0);
    const uint32_t gate = object_raw_field(0, 0xBCU);
    uint32_t parameter = effect;
    switch (blend) {
        case 1: case 0x20: case 0x21: case 0x22: case 0x23: case 0x24:
            parameter = 0x100U - (((0x100U - threshold) * (0x100U - effect) * gate) >> 16U);
            break;
        case 2: case 3: case 4: case 0xC0: case 0xC1:
            parameter = ((0x100U - threshold) * effect * gate) >> 16U;
            break;
        default: break;
    }
    const auto draw = [&](uint32_t bitmap, uint32_t x, uint32_t y, uint32_t mode, uint32_t value) {
        const bool result = record_bitmap_blit(destination, bitmap,
            // 00422840 passes -(field + local dirty origin) to0040B1A0.
            static_cast<int32_t>(0U - x - static_cast<uint32_t>(callback.destination_left)),
            static_cast<int32_t>(0U - y - static_cast<uint32_t>(callback.destination_top)), mode, value);
        return result || fail("BackF bitmap blit rejected its format, mode, parameter or pixels");
    };
    const bool sentinel = c.secondary_bitmap == 0x7000U || c.secondary_bitmap == 0x7001U ||
        c.secondary_bitmap == 0x7FFFU || c.secondary_bitmap == 0xFFFFFFFFU;
    if (c.mask_bitmap != 0xFFFFFFFFU) {
        // PE64 140058910: the dirty rectangle crops the mask before
        // 14004EA70(flag1), independently of the primary source offset.
        const auto mask=graphics_surfaces_.find(c.mask_bitmap);
        if (mask==graphics_surfaces_.end() ||
            graphics_bitmap_token(c.mask_bitmap)!=background_bitmap_tokens_[2]) return clear();
        auto& out=target->second;auto& primary=first->second;auto& plane=mask->second;
        // 140058CE0's default extra-opacity is0. The separate nonzero
        // extra-opacity route remains unresolved. Both original mask families
        // are implemented for this default route.
        if (out.format!=1U || primary.format!=1U || plane.format!=3U)
            return fail("BackF masked rendering requires format1 source/destination and format3 mask");
        if (!materialize_graphics_pixels(primary) || !materialize_graphics_pixels(plane))
            return fail("BackF source or mask pixels could not be materialized");
        if (!sentinel) {
            if (graphics_surfaces_.count(c.secondary_bitmap)==0 ||
                graphics_bitmap_token(c.secondary_bitmap)!=background_bitmap_tokens_[1]) return clear();
            if (!draw(c.secondary_bitmap,c.field4,c.field5,0x80U,0)) return false;
        } else if (c.secondary_bitmap==0x7000U || c.secondary_bitmap==0x7001U) {
            const uint8_t color=c.secondary_bitmap==0x7001U?0xFFU:0;
            for (size_t row=0;row<out.height;++row)
                for (size_t column=0;column<out.width;++column) {
                    const size_t offset=row*out.stride+column*4U;
                    out.pixels[offset]=color;out.pixels[offset+1]=color;
                    out.pixels[offset+2]=color;out.pixels[offset+3]=0;
                }
        }
        const bool composed=compose_original_background_mask(
            {out.pixels,0,out.stride,out.width,out.height,out.format,out.bytes_per_pixel},
            {primary.pixels,0,primary.stride,primary.width,primary.height,primary.format,primary.bytes_per_pixel},
            {plane.pixels,0,plane.stride,plane.width,plane.height,plane.format,plane.bytes_per_pixel},
            static_cast<int32_t>(0U-c.field1-static_cast<uint32_t>(callback.destination_left)),
            static_cast<int32_t>(0U-c.field2-static_cast<uint32_t>(callback.destination_top)),
            callback.destination_left,callback.destination_top,c.field8,parameter);
        if (composed) refresh_bgra_mirror(out);
        return composed || fail("BackF mask kernel rejected its pixel views or parameter");
    }
    if (!sentinel) {
        if (graphics_surfaces_.count(c.secondary_bitmap) == 0 ||
            graphics_bitmap_token(c.secondary_bitmap) != background_bitmap_tokens_[1]) return clear();
        // 00422840: copy the secondary first, then overlay primary in mode1.
        return draw(c.secondary_bitmap, c.field4, c.field5, 0x80U, 0) &&
            draw(c.primary_bitmap, c.field1, c.field2, 1, parameter);
    }
    const auto [main_width, main_height] = current_mode_dimensions();
    const bool fill = (c.secondary_bitmap == 0x7000U || c.secondary_bitmap == 0x7001U) &&
        (static_cast<int32_t>(c.field1) < 0 || static_cast<int32_t>(c.field2) < 0 ||
         static_cast<uint32_t>(first->second.width - c.field1) < main_width ||
         static_cast<uint32_t>(first->second.height - c.field2) < main_height);
    uint32_t mode = parameter == 0 ? 0x80U : 0xC0U;
    if (fill) {
        // 0040B3D0 fills only the callback view; all four bytes are the
        // low24-bit constant (black0 or white00FFFFFF).
        auto& out = target->second;
        const uint8_t color = c.secondary_bitmap == 0x7001U ? 0xFFU : 0;
        for (std::size_t y = 0; y < out.height; ++y)
            for (std::size_t x = 0; x < out.width; ++x) {
                const std::size_t offset = y * out.stride + x * out.bytes_per_pixel;
                out.pixels[offset] = color; out.pixels[offset + 1] = color; out.pixels[offset + 2] = color;
                if (out.bytes_per_pixel == 4) out.pixels[offset + 3] = 0;
            }
        refresh_bgra_mirror(out);
        mode = c.secondary_bitmap == 0x7001U ? 0xC1U : 0xC0U;
    }
    return draw(c.primary_bitmap, c.field1, c.field2, mode, parameter);
}
} // namespace bgiplayer
