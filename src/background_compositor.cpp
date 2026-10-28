#include "bgiplayer/vm.hpp"
#include "bgiplayer/gpu_pixel_kernels.hpp"
#include "bgiplayer/background_motion.hpp"
#include "bgiplayer/background_transform.hpp"

#include <cstdlib>
#include <iostream>
#include <set>
#include <sstream>

namespace bgiplayer {

void VM::refresh_main_background_geometry() {
    const auto [width, height] = current_mode_dimensions();
    update_display_object_geometry(main_background_handle, 0, 0, width, height,
                                   render_control_first_ != 0U);
}

bool VM::compose_main_background_at(uint32_t destination,
                                    const DisplayBitmapDraw& callback) {
    const auto fail = [&](std::string_view reason, uint32_t branch = 0) {
        return fail_display_draw(destination, main_background_handle, "background", reason, branch);
    };
    if (debug_trace(DebugTrace::Display)) {
        std::ostringstream state;
        state << "MAIN_BACKGROUND_RENDER type=" << background_type_
              << " render=" << background_render_enabled_
              << " enabled=" << object_enabled(0) << ',' << object_base_draw_enabled(0)
              << " suppression=" << object_raw_field(0,0xCU)
              << " effect=" << object_effect_level(0)
              << " threshold=" << object_visibility_threshold(0)
              << " gate=" << object_raw_field(0,0xBCU);
        if (const auto f = sprite_mask_configs_.find(0); f != sprite_mask_configs_.end()) {
            state << " primary=" << f->second.primary_bitmap
                  << " token=" << background_bitmap_tokens_[0]
                  << " live=" << graphics_bitmap_token(f->second.primary_bitmap)
                  << " exists=" << graphics_surfaces_.contains(f->second.primary_bitmap);
        }
        static std::string previous;
        if (state.str() != previous) { previous = state.str(); std::cout << previous << '\n'; }
    }
    // 004215D0 clears immediately when the common +148 gate is zero,
    // regardless of subclass. Enabled subclasses with a different +84
    // renderer remain unresolved here.
    if (background_type_ != 1U && background_type_ != 4U && background_type_ != 12U && background_render_enabled_ != 0U) {
        return fail("background subclass rendering is not implemented", background_type_);
    }
    auto target = graphics_surfaces_.find(destination);
    if (target == graphics_surfaces_.end() ||
        !materialize_graphics_pixels(target->second)) {
        return fail("background destination bitmap is missing or cannot be materialized");
    }
    if (background_type_ == 4U && background_render_enabled_ != 0U) {
        return compose_background_f_at(destination, callback);
    }
    if (background_type_ == 12U && background_render_enabled_ != 0U) {
        if (debug_trace(DebugTrace::Display)) {
            static std::set<std::string> recorded;
            for (std::size_t slot_index = 0; slot_index < background_effect_slots_.size(); ++slot_index) {
                const auto& slot = background_effect_slots_[slot_index];
                if (slot.field_150 == 0 && slot.field_154 == 0) continue;
                const auto bitmap = graphics_surfaces_.find(static_cast<uint32_t>(slot.bitmap));
                std::ostringstream state;
                state << "ML_RENDER_STATE slot=" << slot_index << " flags=" << slot.field_150 << ',' << slot.field_154
                      << " bitmap=" << slot.bitmap << " token=" << slot.bitmap_token
                      << " live_token=" << graphics_bitmap_token(static_cast<uint32_t>(slot.bitmap))
                      << " exists=" << (bitmap != graphics_surfaces_.end())
                      << " target_format=" << target->second.format
                      << " position=" << background_position_[0] << ',' << background_position_[1]
                      << " fixed=" << slot.fixed_x << ',' << slot.fixed_y
                      << " origin=" << slot.field_170 << ',' << slot.field_174
                      << " angle=" << slot.field_178 << " scale=" << slot.field_17C << ',' << slot.field_180
                      << " mode=" << slot.mode << " parameter=" << slot.field_164
                      << " motion=" << slot.field_190 << ',' << slot.field_194
                      << " phase=" << object_raw_field(0, 0xC0U)
                      << " selector=" << slot.field_184;
                if (bitmap != graphics_surfaces_.end()) {
                    state << " source_format=" << bitmap->second.format
                          << " size=" << bitmap->second.width << ',' << bitmap->second.height
                          << " source=" << bitmap->second.archive << ':' << bitmap->second.resource;
                }
                if (recorded.insert(state.str()).second) std::cout << state.str() << '\n';
            }
        }
        // 004234A0 visits eight slots. This handles its covering slot-zero
        // integer/unit-scale branch, including 004199D0's exact-copy classifier.
        // Additional live layers and general transformed sampling remain unresolved.
        for (std::size_t index = 1; index < background_effect_slots_.size(); ++index) {
            const auto& layer = background_effect_slots_[index];
            if (layer.field_150 != 0 && layer.field_154 != 0 &&
                graphics_surfaces_.count(static_cast<uint32_t>(layer.bitmap)) != 0 &&
                graphics_bitmap_token(static_cast<uint32_t>(layer.bitmap)) == layer.bitmap_token)
                return fail("multiple active background layers are not implemented", static_cast<uint32_t>(index));
        }
        auto& output = target->second;
        const auto clear_output = [&] {
            const std::size_t bytes = static_cast<std::size_t>(output.width) * output.bytes_per_pixel;
            if (output.stride < bytes || output.pixels.size() <
                    static_cast<std::size_t>(output.stride) * output.height)
                return fail("background clear destination stride or capacity is invalid");
            for (std::size_t row = 0; row < output.height; ++row)
                std::fill_n(output.pixels.begin() + static_cast<std::ptrdiff_t>(row * output.stride),
                            bytes, uint8_t{0});
            refresh_bgra_mirror(output);
            return true;
        };
        const auto& layer = background_effect_slots_[0];
        const uint32_t image = static_cast<uint32_t>(layer.bitmap);
        const auto input = graphics_surfaces_.find(image);
        if (layer.field_150 == 0 || layer.field_154 == 0 ||
            input == graphics_surfaces_.end() || graphics_bitmap_token(image) != layer.bitmap_token)
            return clear_output();
        if (input->second.format != 1U || output.format != 1U)
            return fail("layered background requires format1 source and destination");
        const auto [main_width, main_height] = current_mode_dimensions();
        // 004234A0 re-evaluates 0041F6A0 for position (+184), angle (+188)
        // and scale (+18C). The fast condition below compares these eased
        // values, not the raw base/velocity fields.
        const auto transform = evaluate_background_transform(
            static_cast<int32_t>(layer.field_170), static_cast<int32_t>(layer.field_174),
            static_cast<int32_t>(layer.field_190), static_cast<int32_t>(layer.field_194),
            static_cast<int32_t>(layer.field_178), static_cast<int32_t>(layer.field_198),
            static_cast<int32_t>(layer.field_17C), static_cast<int32_t>(layer.field_180),
            static_cast<int32_t>(layer.field_19C), static_cast<int32_t>(layer.field_1A0),
            static_cast<int32_t>(object_raw_field(0, 0xC0U)),
            layer.field_184, layer.field_188, layer.field_18C);
        if (!transform) return fail("background transform selector or parameters are unsupported");
        // 0041B350 turns every non-identity case into the general transform
        // sampler after 004199D0 rejects the exact-copy classifier. The
        // destination offsets are the fixed slot origin minus the local dirty
        // rectangle. 004213B0 already subtracted the object's integer
        // absolute origin before dispatch, while this callback still carries
        // absolute screen coordinates; the fast arm applies the same
        // background_position_ shift in its blit expression.
        const int32_t screen_origin_x = background_position_[0] >> 16;
        const int32_t screen_origin_y = background_position_[1] >> 16;
        const int32_t destination_x = static_cast<int32_t>(
            layer.fixed_x -
            static_cast<uint32_t>(callback.destination_left - screen_origin_x) * 0x10000U);
        const int32_t destination_y = static_cast<int32_t>(
            layer.fixed_y -
            static_cast<uint32_t>(callback.destination_top - screen_origin_y) * 0x10000U);
        if (debug_trace(DebugTrace::Display)) {
            static std::set<std::string> recorded_transforms;
            std::ostringstream state;
            state << "ML_TRANSFORM phase=" << object_raw_field(0, 0xC0U)
                  << " origin=" << transform->origin_x << ',' << transform->origin_y
                  << " destination=" << destination_x << ',' << destination_y
                  << " angle=" << transform->angle
                  << " scale=" << transform->scale_x << ',' << transform->scale_y
                  << " selectors=" << layer.field_184 << ',' << layer.field_188
                  << ',' << layer.field_18C << " option=" << layer.field_1A4
                  << " parameter=" << layer.field_164;
            if (recorded_transforms.insert(state.str()).second) std::cout << state.str() << '\n';
        }
        const auto compose_general = [&]() -> bool {
            // 0041B350's non-zero parameter selects the 0041B5B0/0041BC10
            // opacity families; option 0 selects the 0041C4E0 nearest kernel.
            if (layer.field_164 != 0) return fail("background transformed opacity kernel is not implemented");
            if (!materialize_graphics_pixels(input->second)) return fail("background source pixels could not be materialized");
            // 004199D0 includes the shifted-rectangle containment test. An
            // integer transform alone does not select 0041D0C0: uncovered
            // edges must still run the sampler and write zero there.
            const auto crop=classify_background_exact_copy(*transform,destination_x,destination_y,
                output.width,output.height,input->second.width,input->second.height);
            if(crop) {
                const auto& source=input->second;
                if(uint64_t(output.width)*4U>output.stride ||
                    uint64_t(source.width)*4U>source.stride ||
                    uint64_t(output.stride)*output.height>output.pixels.size() ||
                    uint64_t(source.stride)*source.height>source.pixels.size())
                    return fail("background exact-copy pixel view is invalid");
                for(uint32_t row=0;row<output.height;++row)
                    std::copy_n(source.pixels.data()+size_t(row+crop->second)*source.stride+
                            size_t(crop->first)*4U,size_t(output.width)*4U,
                            output.pixels.data()+size_t(row)*output.stride);
                refresh_bgra_mirror(output);
                return true;
            }
            if (layer.field_1A4 == 0 && !compact_64_opcodes_)
                return fail("legacy background nearest transform kernel is not implemented");
            const auto affine = layer.field_1A4 == 0
                ? evaluate_pe64_background_affine(*transform, destination_x, destination_y)
                : evaluate_background_affine(*transform, destination_x, destination_y);
            if (!affine) return fail("background affine coefficients could not be evaluated");
            const auto sampler = layer.field_1A4 == 0
                ? sample_background_transform_nearest : sample_background_transform;
            if (!sampler(output.pixels.data(), output.stride, output.width, output.height,
                    input->second.pixels.data(), input->second.stride,
                    input->second.width, input->second.height, *affine))
                return fail("background transformed sampler rejected its pixel views or geometry");
            refresh_bgra_mirror(output);
            return true;
        };
        const int32_t delta_x = static_cast<int32_t>(layer.fixed_x - static_cast<uint32_t>(transform->origin_x));
        const int32_t delta_y = static_cast<int32_t>(layer.fixed_y - static_cast<uint32_t>(transform->origin_y));
        // ML +2C(004232C0) stores the common Q16 position at+54/+58.
        // Dispatch004213B0 subtracts its integer absolute origin from the
        // local dirty rectangle while the background bounds remain full-main.
        const int32_t origin_x = background_position_[0] >> 16;
        const int32_t origin_y = background_position_[1] >> 16;
        const int64_t global_x = static_cast<int64_t>(delta_x >> 16) + origin_x;
        const int64_t global_y = static_cast<int64_t>(delta_y >> 16) + origin_y;
        if ((background_position_[0] & 0xFFFF) != 0 || (background_position_[1] & 0xFFFF) != 0 ||
            (delta_x & 0xFFFF) != 0 || (delta_y & 0xFFFF) != 0 ||
            global_x > 0 || global_y > 0 || transform->angle != 0 ||
            transform->scale_x != 0x10000 || transform->scale_y != 0x10000 ||
            layer.field_164 != 0 ||
            static_cast<int64_t>(input->second.width) + global_x < main_width ||
            static_cast<int64_t>(input->second.height) + global_y < main_height)
            return compose_general();
        if (!clear_output()) return false;
        if (!record_bitmap_blit(destination, image,
            (delta_x >> 16) - (callback.destination_left - origin_x),
            (delta_y >> 16) - (callback.destination_top - origin_y), 5U, layer.field_164))
            return fail("background covering-layer bitmap blit rejected its mode, parameter or pixels");
        return true;
    }
    const uint32_t bitmap = static_cast<uint32_t>(background_bitmaps_[0]);
    auto source = graphics_surfaces_.find(bitmap);
    // 00423DB0 compares the allocation token, not the bitmap's width.
    // 00409160 returns manager-entry+1C; 00408BE0 writes the token there.
    if (background_render_enabled_ == 0U ||
        background_bitmaps_[0] < 0 || source == graphics_surfaces_.end() ||
        graphics_bitmap_token(bitmap) != background_bitmap_tokens_[0]) {
        // 004215D0 -> 0040B270(target,nullptr): zero each visible row,
        // retaining any row padding, without alpha blending.
        auto& output = target->second;
        const std::size_t row_bytes =
            static_cast<std::size_t>(output.width) * output.bytes_per_pixel;
        if (output.stride < row_bytes ||
            output.pixels.size() <
                static_cast<std::size_t>(output.stride) * output.height) {
            return fail("background clear destination stride or capacity is invalid");
        }
        if(compose_host_pixel_transfer({{output.pixels,0,output.stride,output.width,output.height,output.format,output.bytes_per_pixel},
                {},PixelTransferOperation::Clear}).success) {
            refresh_bgra_mirror(output);return true;
        }
        for (std::size_t row = 0; row < output.height; ++row) {
            std::fill_n(output.pixels.begin() +
                            static_cast<std::ptrdiff_t>(row * output.stride),
                        row_bytes, uint8_t{0});
        }
        refresh_bgra_mirror(output);
        return true;
    }
    if (!materialize_graphics_pixels(source->second)) {
        return fail("background source pixels could not be materialized");
    }

    // 00423DB0 crops the source descriptor with 00409CB0, then invokes
    // 0040B760(mode=80h,param=0,threading=1). It deliberately ignores the
    // crop's return value: an empty intersection leaves the source intact.
    const auto& input = source->second;
    int32_t left = std::max<int32_t>(0, callback.destination_left);
    int32_t top = std::max<int32_t>(0, callback.destination_top);
    int32_t right = std::min<int32_t>(
        static_cast<int32_t>(input.width - 1U), callback.destination_right);
    int32_t bottom = std::min<int32_t>(
        static_cast<int32_t>(input.height - 1U), callback.destination_bottom);
    if (left > right || top > bottom) {
        left = top = 0;
        right = static_cast<int32_t>(input.width - 1U);
        bottom = static_cast<int32_t>(input.height - 1U);
    }
    GraphicsSurface view;
    view.width = static_cast<uint32_t>(right - left) + 1U;
    view.height = static_cast<uint32_t>(bottom - top) + 1U;
    view.format = input.format;
    view.bytes_per_pixel = input.bytes_per_pixel;
    view.stride = view.width * view.bytes_per_pixel;
    view.pixels.resize(static_cast<std::size_t>(view.stride) * view.height);
    const std::size_t base = static_cast<std::size_t>(top) * input.stride +
        static_cast<std::size_t>(left) * input.bytes_per_pixel;
    if (input.stride <
            static_cast<std::size_t>(input.width) * input.bytes_per_pixel ||
        input.pixels.size() <
            static_cast<std::size_t>(input.stride) * input.height) {
        return fail("background source stride or pixel capacity is invalid");
    }
    for (std::size_t row = 0; row < view.height; ++row) {
        std::copy_n(input.pixels.begin() +
                        static_cast<std::ptrdiff_t>(base + row * input.stride),
                    view.stride,
                    view.pixels.begin() +
                        static_cast<std::ptrdiff_t>(row * view.stride));
    }
    uint32_t temporary = 0xFFFFFFFFU;
    while (graphics_surfaces_.count(temporary) != 0) {
        --temporary;
    }
    graphics_surfaces_.emplace(temporary, std::move(view));
    try {
        const bool result = record_bitmap_blit(
            destination, temporary, 0, 0, 0x80U, 0U);
        graphics_surfaces_.erase(temporary);
        if (!result) return fail("background cropped bitmap copy failed");
        return true;
    } catch (...) {
        graphics_surfaces_.erase(temporary);
        throw;
    }
}

}  // namespace bgiplayer
