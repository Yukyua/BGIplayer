#include "bgiplayer/vm.hpp"
#include "bgiplayer/sprite_crossfade.hpp"
#include "bgiplayer/sprite_scaled_alpha.hpp"
#include "bgiplayer/sprite_rotated_alpha.hpp"
#include "bgiplayer/sprite_reduced_cache.hpp"
#include "bgiplayer/sprite_terminal_blend.hpp"
#include "bgiplayer/background_transform.hpp"
#include "bgiplayer/gpu_pixel_kernels.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <set>

namespace bgiplayer {

bool VM::compose_single_bitmap_sprite_at(uint32_t destination, uint32_t handle,
                                         int32_t x, int32_t y) {
    const auto found = sprites_.find(handle);
    const auto fail = [&](std::string_view reason, uint32_t branch = 0) {
        return fail_display_draw(destination, handle, "sprite", reason, branch);
    };
    if (found == sprites_.end()) return fail("sprite state is missing");
    if (!found->second.configured) return fail("sprite is not configured");
    if (graphics_surfaces_.count(destination) == 0) return fail("destination bitmap is missing");
    // 0042AC5B..0042ACAB reduces identity raw5 to raw0 for one source
    // and raw1 for two sources. PE64 140065CC9..140065D1F agrees.
    const bool identity_mode5 = object_raw_field(handle, 0x144U) == 5U &&
        object_raw_field(handle, 0x2A4U) == 0U &&
        object_raw_field(handle, 0x2B4U) == 0x10000U &&
        object_raw_field(handle, 0x2B8U) == 0x10000U &&
        object_raw_field(handle, 0x2BCU) == 0U &&
        object_raw_field(handle, 0x2C0U) == 0U &&
        (object_raw_field(handle, 0x318U) == 0U ||
         object_raw_field(handle, 0x320U) == 0U);
    if (debug_trace(DebugTrace::Display) && sprite_mode_configurations_.count(handle) != 0) {
        const std::array<uint32_t, 12> state{handle, object_raw_field(handle, 0x144U),
            found->second.bitmap, object_raw_field(handle, 0x164U), object_raw_field(handle, 0x2A4U),
            object_raw_field(handle, 0x2B4U), object_raw_field(handle, 0x2B8U),
            object_raw_field(handle, 0x2BCU), object_raw_field(handle, 0x2C0U),
            object_raw_field(handle, 0x318U), object_raw_field(handle, 0x320U), identity_mode5};
        static std::set<std::array<uint32_t, 12>> reported;
        if (reported.insert(state).second)
            std::cout << "SPRITE_ADVANCED_ROUTE object=" << handle << " raw_mode=" << state[1]
                      << " api_mode=" << sprite_mode_configurations_.at(handle).mode
                      << " source=" << state[2] << " second=" << state[3] << " angle=" << state[4]
                      << " scale=" << state[5] << ',' << state[6] << " fraction=" << state[7] << ',' << state[8]
                      << " stretch=" << state[9] << ',' << state[10] << " identity=" << identity_mode5
                      << " base_angle=" << static_cast<int32_t>(object_raw_field(handle,0x260U))
                      << " angle_coefficient=" << static_cast<int32_t>(object_raw_field(handle,0x2CCU))
                      << " phase=" << static_cast<int32_t>(object_raw_field(handle,0xC0U))
                      << " easing=" << object_raw_field(handle,0x2E8U) << '\n';
    }
    const bool plain_crossfade = object_raw_field(handle,0x144U) == 1U ||
        (identity_mode5 && object_raw_field(handle,0x164U) != 0xFFFFFFFFU);
    const bool scaled_single =
        object_raw_field(handle,0x144U) == 5U && !identity_mode5 &&
        object_raw_field(handle,0x164U) == 0xFFFFFFFFU &&
        object_raw_field(handle,0x2B4U) > 0U &&
        object_raw_field(handle,0x2B8U) > 0U &&
        (object_raw_field(handle,0x318U) == 0U ||
         object_raw_field(handle,0x320U) == 0U);
    const bool scaled_pair =
        object_raw_field(handle,0x144U) == 5U && !identity_mode5 &&
        object_raw_field(handle,0x164U) != 0xFFFFFFFFU &&
        object_raw_field(handle,0x2B4U) > 0U &&
        object_raw_field(handle,0x2B8U) > 0U &&
        (object_raw_field(handle,0x318U) == 0U ||
         object_raw_field(handle,0x320U) == 0U);
    // Raw0 (including identity raw5) returns without drawing when its
    // descriptor no longer exists: old32 42AD07..26, PE64 65D89..D9F.
    // Scaled raw5 has its own identical descriptor guard below. A missing
    // source is a completed no-op, not a failure of the whole display frame.
    if ((object_raw_field(handle, 0x144U) == 0U || identity_mode5) &&
        !plain_crossfade && graphics_surfaces_.count(found->second.bitmap) == 0) {
        return true;
    }
    if ((sprite_mode_configurations_.count(handle) != 0 && !identity_mode5 &&
         !plain_crossfade && !scaled_single && !scaled_pair) ||
        sprite_mask_configs_.count(handle) != 0) {
        if (sprite_mask_configs_.count(handle) != 0)
            return fail("sprite mask rendering is not implemented");
        const uint32_t raw = object_raw_field(handle, 0x144U);
        if (raw != 5U) return fail("raw sprite drawing mode is not implemented");
        if (object_raw_field(handle, 0x2B4U) == 0U || object_raw_field(handle, 0x2B8U) == 0U)
            return fail("zero-scale sprite geometry is not implemented");
        return fail("sprite stretch rendering is not implemented");
    }
    if (object_enabled(handle) == 0 || object_base_draw_enabled(handle) == 0 ||
        object_raw_field(handle, 0x0CU) != 0 ||
        object_visibility_threshold(handle) >= 0x100U ||
        object_raw_field(handle, 0xBCU) == 0) {
        return true;
    }
    const uint32_t mode = object_raw_field(handle, 0xB0U);
    const uint32_t effect = object_effect_level(handle);
    const uint32_t brightness = object_visibility_threshold(handle);
    const uint32_t gate = object_raw_field(handle, 0xBCU);
    uint32_t parameter = effect;
    switch (mode) {
        case 1: case 0x20: case 0x21: case 0x22: case 0x23: case 0x24:
            parameter = 0x100U -
                (((0x100U - brightness) * (0x100U - effect) * gate) >> 16U);
            break;
        case 2: case 3: case 4: case 0xC0: case 0xC1:
            parameter = ((0x100U - brightness) * effect * gate) >> 16U;
            break;
        default:
            break;
    }
    if (scaled_single || scaled_pair) {
        // PE64 140061AE0 / PE32 0042F640 build up to four reduced source levels. 140061780
        // requests quantization from scaleX only, while 1400616A0 caps it to
        // available levels. Pair caching then mixes the selected level before
        // the shared rotated/unrotated affine sampler.
        if (object_raw_field(handle,0x158U) != 0U) return true;
        const auto first = graphics_surfaces_.find(object_raw_field(handle,0x160U));
        if (first == graphics_surfaces_.end() ||
            first->second.allocation_token != object_raw_field(handle,0x168U)) return true;
        if (object_raw_field(handle,0x148U) != 0U)
            return fail("sprite auxiliary rendering mode is not implemented", object_raw_field(handle,0x148U));
        const bool terminal_add_sub = compact_64_opcodes_ &&
            (mode == 2U || mode == 3U || mode == 0x21U || mode == 0x22U);
        if (mode != 0U && mode != 1U && mode != 0x20U && !terminal_add_sub)
            return fail("sprite terminal blend mode is not implemented");
        // 1400615A0 color modulation selects a separate original branch.
        for (uint32_t block=0;block<4U;++block)
            for (uint32_t offset : {0x370U,0x37CU,0x388U,0x394U})
                if (object_raw_field(handle,offset+block*0x30U) != 0U)
                    return fail("sprite color modulation is not implemented", offset+block*0x30U);
        uint32_t quantization=0, threshold=0x8000U;
        uint32_t available_width=first->second.width, available_height=first->second.height;
        const uint32_t raw_scale_x=object_raw_field(handle,0x2B4U);
        while (quantization<4U && raw_scale_x<=threshold && available_width>=2U && available_height>=2U) {
            ++quantization; threshold>>=1U;
            available_width=(available_width+1U)>>1U;
            available_height=(available_height+1U)>>1U;
        }
        const auto reduce_source=[&](GraphicsSurface& input, GraphicsSurface& result) {
            if (!materialize_graphics_pixels(input)) return false;
            GraphicsSurface* previous=&input;
            for (uint32_t level=0;level<quantization;++level) {
                GraphicsSurface next;next.width=(previous->width+1U)>>1U;next.height=(previous->height+1U)>>1U;
                next.stride=next.width*4U;next.format=previous->format;next.bytes_per_pixel=4U;
                next.pixels.resize(size_t(next.stride)*next.height);
                if (!reduce_original_sprite_cache({next.pixels,0,next.stride,next.width,next.height,next.format,4},
                        {previous->pixels,0,previous->stride,previous->width,previous->height,previous->format,4}))
                    return false;
                result=std::move(next);previous=&result;
            }
            return true;
        };
        GraphicsSurface mixed,reduced_first,reduced_second;
        GraphicsSurface* first_source=&first->second;
        if (quantization) {
            if (!reduce_source(first->second,reduced_first))
                return fail("original sprite reduced-cache kernel rejected its source layout");
            first_source=&reduced_first;
        }
        if (scaled_pair) {
            // 140063500 builds a format2 cache with 1400416A0 -> 14003C5C0;
            // 140066895 then samples that cache through the single-source
            // affine kernel. Mix before filtering, preserving alpha.
            const auto second=graphics_surfaces_.find(object_raw_field(handle,0x164U));
            if (second==graphics_surfaces_.end() ||
                second->second.allocation_token!=object_raw_field(handle,0x16CU)) return true;
            if (first->second.width!=second->second.width || first->second.height!=second->second.height)
                return fail("mixed sprite source dimensions differ");
            GraphicsSurface* second_source=&second->second;
            if (quantization) {
                if (!reduce_source(second->second,reduced_second))
                    return fail("original sprite reduced-cache kernel rejected its secondary source layout");
                second_source=&reduced_second;
            }
            auto& a=*first_source;auto& b=*second_source;
            if (a.format!=2U || b.format!=2U) return fail("mixed sprite cache requires two format2 sources");
            if (a.width!=b.width || a.height!=b.height) return fail("mixed sprite source dimensions differ");
            if (!materialize_graphics_pixels(a) || !materialize_graphics_pixels(b))
                return fail("mixed sprite source pixels could not be materialized");
            mixed.width=a.width;mixed.height=a.height;mixed.stride=a.width*4U;
            mixed.format=2U;mixed.bytes_per_pixel=4U;
            mixed.pixels.resize(size_t(mixed.stride)*mixed.height);
            if (!blend_original_sprite_cache(
                    {mixed.pixels,0,mixed.stride,mixed.width,mixed.height,2,4},
                    {a.pixels,0,a.stride,a.width,a.height,2,4},
                    {b.pixels,0,b.stride,b.width,b.height,2,4},
                    static_cast<int16_t>(object_raw_field(handle,0x250U))))
                return fail("original sprite cache blend rejected its pixel views or mix");
        }
        auto& out = graphics_surfaces_.at(destination);
        auto& source = scaled_pair ? mixed : *first_source;
        if (out.format != 1U || source.format != 2U)
            return fail("transformed sprite requires format2 source and format1 destination");
        if (!materialize_graphics_pixels(out) || !materialize_graphics_pixels(source))
            return fail("transformed sprite pixels could not be materialized");
        // Callback destination is the clipped raster-local rectangle. The
        // original passes (raster origin - clip.left/top) plus Q16 fraction,
        // not the sprite's world anchor, to 140052DE0.
        const auto position_q16 = [this,handle](int32_t clip_offset,
                                               uint32_t origin_offset,
                                               uint32_t fraction_offset) {
            return static_cast<int32_t>(((static_cast<uint32_t>(clip_offset) +
                object_raw_field(handle,origin_offset)) << 16U) +
                object_raw_field(handle,fraction_offset));
        };
        // 1400668AD shifts source origins right and scales left by the
        // actual cache level. 1400668C4's correction loop uses scaleX for
        // BOTH origins, including when fewer than the requested levels exist.
        const uint32_t sx=raw_scale_x<<quantization;
        const uint32_t sy=object_raw_field(handle,0x2B8U)<<quantization;
        uint32_t origin_correction=0, residual=sx, correction_shift=1;
        while (residual && residual<0x10000U) {
            origin_correction+=(residual<0x8000U?0x8000U:0x10000U-residual)>>correction_shift;
            residual<<=1U;++correction_shift;
        }
        const int32_t origin_x=static_cast<int32_t>(uint32_t(static_cast<int32_t>(object_raw_field(handle,0x29CU))>>quantization)+origin_correction);
        const int32_t origin_y=static_cast<int32_t>(uint32_t(static_cast<int32_t>(object_raw_field(handle,0x2A0U))>>quantization)+origin_correction);
        const int32_t angle=static_cast<int32_t>(object_raw_field(handle,0x2A4U));
        // Quantization depends only on X, and small sources can exhaust
        // available levels. Either residual scale may therefore still be at
        // or below50%; the general original affine kernel admits it even
        // though the specialized host scaled-alpha helper does not.
        if (terminal_add_sub) {
            // 140066988..1400669FE samples every BGRA byte into a format2
            // temporary through52B50(parameter0), then common3E490 performs
            // the terminal blend. It does not alpha-compose into the target
            // before applying the add/subtract operation.
            if (out.width == 0U || out.height == 0U || out.width > 32767U ||
                out.height > 32767U || source.width == 0U || source.height == 0U ||
                source.width > 32767U || source.height > 32767U ||
                source.stride < uint64_t(source.width) * 4U ||
                source.pixels.size() < uint64_t(source.stride) * source.height)
                return fail("sprite terminal sampling rejected its pixel views");
            const auto matrix = inverse_sprite_affine_q16(
                position_q16(x,0x2F4U,0x2BCU),position_q16(y,0x2F8U,0x2C0U),
                origin_x,origin_y,angle,sx,sy);
            if (!matrix) return fail("sprite terminal sampling rejected its geometry");
            const BackgroundAffine affine{(*matrix)[0],(*matrix)[1],(*matrix)[2],
                (*matrix)[3],(*matrix)[4],(*matrix)[5]};
            const uint32_t stride = out.width * 4U;
            std::vector<uint8_t> sampled(size_t(stride) * out.height);
            const auto sampler = object_raw_field(handle,0x298U) != 0U ?
                sample_background_transform : sample_background_transform_nearest;
            if (!sampler(sampled.data(),stride,out.width,out.height,
                    source.pixels.data(),source.stride,source.width,source.height,affine) ||
                !blend_original_sprite_terminal(
                    {out.pixels,0,out.stride,out.width,out.height,1,4},
                    {sampled,0,stride,out.width,out.height,2,4},mode,parameter))
                return fail("sprite terminal add/subtract rejected its pixel views or parameter");
        } else if (angle!=0 || sx<=0x8000U || sy<=0x8000U) {
            if (!compose_rotated_alpha_bgra(out.pixels,out.width,out.height,out.stride,
                    source.pixels,source.width,source.height,source.stride,
                    position_q16(x,0x2F4U,0x2BCU),position_q16(y,0x2F8U,0x2C0U),
                    origin_x,origin_y,angle,
                    sx,sy,
                    parameter,object_raw_field(handle,0x298U)!=0U))
                return fail("rotated sprite kernel rejected its pixel views or geometry");
        } else if (!compose_host_scaled_alpha({out.pixels,out.width,out.height,out.stride,
                source.pixels,source.width,source.height,source.stride,
                position_q16(x,0x2F4U,0x2BCU),position_q16(y,0x2F8U,0x2C0U),
                origin_x,origin_y,
                sx,sy,
                parameter,object_raw_field(handle,0x298U) != 0U}).success)
            return fail("scaled sprite kernel rejected its pixel views or geometry");
        refresh_bgra_mirror(out);
        display_dirty_=true;
        return true;
    }
    if (plain_crossfade) {
        // 0042AFB9..0042B02C: missing/stale bitmap descriptors draw nothing.
        if (object_raw_field(handle,0x158U) != 0U) return true;
        auto first = graphics_surfaces_.find(object_raw_field(handle,0x160U));
        auto second = graphics_surfaces_.find(object_raw_field(handle,0x164U));
        if (first == graphics_surfaces_.end() || second == graphics_surfaces_.end() ||
            first->second.allocation_token != object_raw_field(handle,0x168U) ||
            second->second.allocation_token != object_raw_field(handle,0x16CU)) return true;
        // Only the proven plain 42B032..42B094 -> 40CED0 branch is supported.
        if (object_raw_field(handle,0x148U) != 0U)
            return fail("sprite auxiliary rendering mode is not implemented", object_raw_field(handle,0x148U));
        if (mode != 0U && mode != 1U && mode != 0x20U)
            return fail("sprite terminal blend mode is not implemented");
        for (uint32_t block=0;block<4U;++block)
            for (uint32_t offset : {0x370U,0x37CU,0x388U,0x394U})
                if (object_raw_field(handle,offset+block*0x30U) != 0U)
                    return fail("sprite color modulation is not implemented", offset+block*0x30U);
        auto& out = graphics_surfaces_.at(destination);
        auto& a = first->second;
        auto& b = second->second;
        if (out.format != 1U || a.format != 2U || b.format != 2U)
            return fail("sprite crossfade requires format2 sources and format1 destination");
        if (!materialize_graphics_pixels(out) || !materialize_graphics_pixels(a) ||
            !materialize_graphics_pixels(b)) return fail("sprite crossfade pixels could not be materialized");
        const int64_t left=std::max<int64_t>(0,x), top=std::max<int64_t>(0,y);
        const int64_t right=std::min<int64_t>(out.width,static_cast<int64_t>(x)+std::min(a.width,b.width));
        const int64_t bottom=std::min<int64_t>(out.height,static_cast<int64_t>(y)+std::min(a.height,b.height));
        if (left>=right || top>=bottom) return true;
        const auto width=static_cast<uint32_t>(right-left), height=static_cast<uint32_t>(bottom-top);
        const auto output_offset=static_cast<uint32_t>(top*out.stride+left*4);
        const auto first_offset=static_cast<uint32_t>((top-y)*a.stride+(left-x)*4);
        const auto second_offset=static_cast<uint32_t>((top-y)*b.stride+(left-x)*4);
        if (!compose_host_sprite_crossfade({{out.pixels,output_offset,out.stride,width,height,1,4},
                {a.pixels,first_offset,a.stride,width,height,2,4},
                {b.pixels,second_offset,b.stride,width,height,2,4},
                static_cast<int16_t>(object_raw_field(handle,0x250U)),mode==0U?0U:parameter}).success)
            return fail("sprite crossfade kernel rejected its pixel views or mix");
        refresh_bgra_mirror(out);
        display_dirty_=true;
        return true;
    }
    if (!record_bitmap_blit(destination, found->second.bitmap, x, y, mode, parameter))
        return fail("sprite bitmap blit rejected its format, blend mode, parameter or pixels");
    return true;
}

bool VM::compose_single_bitmap_sprite(uint32_t destination, uint32_t handle) {
    const auto found = sprites_.find(handle);
    if (found == sprites_.end()) {
        return fail_display_draw(destination, handle, "sprite", "sprite state is missing");
    }
    uint32_t x = static_cast<uint32_t>(found->second.x);
    uint32_t y = static_cast<uint32_t>(found->second.y);
    for (const auto* offsets : {&object_pair_3c_40_, &object_pair_44_48_}) {
        if (const auto offset = offsets->find(handle); offset != offsets->end()) {
            x += static_cast<uint32_t>(offset->second.first);
            y += static_cast<uint32_t>(offset->second.second);
        }
    }
    if (object_raw_field(handle, 0x4CU) != 0) {
        x += static_cast<uint32_t>(display_transform_origin_x_);
        y += static_cast<uint32_t>(display_transform_origin_y_);
    }
    return compose_single_bitmap_sprite_at(destination, handle,
        static_cast<int32_t>(x), static_cast<int32_t>(y));
}


bool VM::execute_sprite_display_callback(const DisplayBitmapDraw& callback) {
    if (sprites_.count(callback.display_object) == 0)
        return fail_display_draw(callback.bitmap, callback.display_object, "sprite", "sprite state is missing");
    return execute_display_callback(callback);
}

bool VM::execute_display_callback(const DisplayBitmapDraw& callback,
                                  std::optional<uint32_t> local_window) {
    struct WarningContextScope {
        std::optional<DisplayBitmapDraw>& slot;
        std::optional<DisplayBitmapDraw> previous;
        ~WarningContextScope() { slot = previous; }
    } warning_context{display_warning_context_, display_warning_context_};
    display_warning_context_ = callback;
    const auto fail = [&](std::string_view reason) {
        return fail_display_draw(callback.bitmap, callback.display_object, "callback", reason);
    };
    auto target = graphics_surfaces_.find(callback.bitmap);
    const bool filter_rgb555 = target!=graphics_surfaces_.end() &&
        target->second.format==0 && target->second.bytes_per_pixel==2 &&
        filter_states_.contains(callback.display_object);
    const uint32_t pixel_size=filter_rgb555?2U:4U;
    if (target == graphics_surfaces_.end() ||
        (target->second.format != 1U && target->second.format != 2U && !filter_rgb555) ||
        callback.source_left < 0 || callback.source_top < 0 ||
        callback.source_right < callback.source_left ||
        callback.source_bottom < callback.source_top ||
        static_cast<uint32_t>(callback.source_right) >= target->second.width ||
        static_cast<uint32_t>(callback.source_bottom) >= target->second.height ||
        !materialize_graphics_pixels(target->second)) {
        if (debug_trace(DebugTrace::Display)) {
            static std::set<uint32_t> reported;
            if (reported.insert(callback.display_object).second) {
                std::cerr << "DISPLAY_TARGET_FAILURE object=" << callback.display_object
                          << " target=" << callback.bitmap << " exists="
                          << (target != graphics_surfaces_.end());
                if (target != graphics_surfaces_.end())
                    std::cerr << " format=" << target->second.format << " size="
                              << target->second.width << ',' << target->second.height;
                std::cerr << '\n';
            }
        }
        if (target == graphics_surfaces_.end()) return fail("callback destination bitmap is missing");
        if (target->second.format != 1U && target->second.format != 2U && !filter_rgb555)
            return fail("callback destination format is not implemented");
        if (callback.source_left < 0 || callback.source_top < 0 ||
            callback.source_right < callback.source_left || callback.source_bottom < callback.source_top ||
            static_cast<uint32_t>(callback.source_right) >= target->second.width ||
            static_cast<uint32_t>(callback.source_bottom) >= target->second.height)
            return fail("callback clip is outside the destination bitmap");
        return fail("callback destination pixels could not be materialized");
    }
    const IconInputIconRecord* icon_record = nullptr;
    const IconInputGroupRecord* icon_group = nullptr;
    std::size_t icon_index = 0;
    uint32_t icon_window = 0;
    for (const auto& [id, processor] : icon_input_processors_) {
        (void)id;
        for (const IconInputGroupRecord& group : processor.layout.groups) {
            for (std::size_t index = 0;
                 index < group.icons.size(); ++index) {
                const IconInputIconRecord& icon = group.icons[index];
                if (icon.display_object_id ==
                    callback.display_object) {
                    icon_record = &icon;
                    icon_group = &group;
                    icon_index = index;
                    icon_window = processor.window;
                    break;
                }
            }
            if (icon_record != nullptr) {
                break;
            }
        }
        if (icon_record != nullptr) {
            break;
        }
    }
    const uint32_t width = static_cast<uint32_t>(callback.source_right - callback.source_left) + 1U;
    const uint32_t height = static_cast<uint32_t>(callback.source_bottom - callback.source_top) + 1U;
    GraphicsSurface view;
    view.width = width;
    view.height = height;
    view.format = target->second.format;
    view.bytes_per_pixel = pixel_size;
    view.stride = width * pixel_size;
    view.pixels.resize(static_cast<std::size_t>(view.stride) * height);
    const std::size_t target_stride = target->second.stride;
    if (target_stride < static_cast<std::size_t>(target->second.width) * pixel_size ||
        target->second.pixels.size() < target_stride * target->second.height) {
        return fail("callback destination stride or pixel capacity is invalid");
    }
    const std::size_t base = static_cast<std::size_t>(callback.source_top) * target_stride +
                             static_cast<std::size_t>(callback.source_left) * pixel_size;
    for (std::size_t row = 0; row < height; ++row) {
        std::copy_n(target->second.pixels.begin() + static_cast<std::ptrdiff_t>(base + row * target_stride),
                    view.stride, view.pixels.begin() + static_cast<std::ptrdiff_t>(row * view.stride));
    }
    uint32_t scratch = 0xFFFFFFFFU;
    while (graphics_surfaces_.count(scratch) != 0) {
        --scratch;
    }
    uint32_t icon_source = scratch - 1U;
    while (graphics_surfaces_.count(icon_source) != 0) {
        --icon_source;
    }
    const std::size_t previous_blits = bitmap_blits_.size();
    const bool previous_dirty = display_dirty_;
    const auto cleanup = [&] {
        graphics_surfaces_.erase(scratch);
        graphics_surfaces_.erase(icon_source);
        bitmap_blits_.resize(previous_blits);
        display_dirty_ = previous_dirty;
    };
    graphics_surfaces_.emplace(scratch, std::move(view));
    try {
        const int32_t x = static_cast<int32_t>(0U - static_cast<uint32_t>(callback.destination_left));
        const int32_t y = static_cast<int32_t>(0U - static_cast<uint32_t>(callback.destination_top));
        bool composed = false;
        const auto failure_serial = display_failure_serial_;
        if (icon_record != nullptr) {
            if(icon_record->render_effect>=256U)composed=true;
            else if (icon_record->animation_angle_q16 != 0) {
                const auto frame =
                    render_icon_frame(*icon_group, icon_index);
                const IconRenderBounds bounds =
                    icon_render_bounds(*icon_group, icon_index);
                if (frame && bounds.right >= bounds.left &&
                    bounds.bottom >= bounds.top) {
                    GraphicsSurface rendered;
                    rendered.width = frame->width;
                    rendered.height = frame->height;
                    rendered.format = 2;
                    rendered.bytes_per_pixel = 4;
                    rendered.stride = frame->width * 4U;
                    rendered.rgba = frame->rgba;
                    rendered.pixels.resize(
                        static_cast<std::size_t>(
                            rendered.stride) *
                            rendered.height,
                        0);
                    for (std::size_t pixel = 0;
                         pixel <
                         static_cast<std::size_t>(
                             frame->width) *
                             frame->height;
                         ++pixel) {
                        const std::size_t offset = pixel * 4U;
                        rendered.pixels[offset] =
                            frame->rgba[offset + 2U];
                        rendered.pixels[offset + 1U] =
                            frame->rgba[offset + 1U];
                        rendered.pixels[offset + 2U] =
                            frame->rgba[offset];
                        rendered.pixels[offset + 3U] =
                            frame->rgba[offset + 3U];
                    }
                    graphics_surfaces_[icon_source] =
                        std::move(rendered);
                    const auto window =
                        window_objects_.find(icon_window);
                    const int32_t window_x =
                        window == window_objects_.end() || local_window
                            ? 0
                            : window->second.x;
                    const int32_t window_y =
                        window == window_objects_.end() || local_window
                            ? 0
                            : window->second.y;
                    const int32_t rotated_x =
                        window_x + bounds.left -
                        callback.source_left;
                    const int32_t rotated_y =
                        window_y + bounds.top -
                        callback.source_top;
                    composed =
                        record_bitmap_blit(
                            scratch, icon_source,
                            rotated_x, rotated_y,
                            icon_record->render_effect?1U:0U,icon_record->render_effect);
                }
            } else {
                const uint32_t source =
                    icon_render_bitmap(*icon_group, icon_index);
                composed =
                    graphics_surfaces_.count(source) != 0 &&
                    // Extended CDspObjSprite: 0042AC00 reduces identity raw5
                    // to raw0, then 0042AF8F calls 0040B760 with terminal
                    // mode 0. Source format2 dispatches to 0040C090/0040C210.
                    // Basic icon 00451970's separate mask40 pass does not
                    // occur here; it would erase the antialiased glow's
                    // background before alpha composition.
                    record_bitmap_blit(
                        scratch, source, x, y,
                        icon_record->render_effect?1U:0U,icon_record->render_effect);
            }
        } else {
            composed = callback.display_object == main_background_handle
                ? compose_main_background_at(scratch, callback)
                : filter_states_.contains(callback.display_object)
                ? compose_filter_at(scratch, callback.display_object,
                                    uint32_t(callback.source_left),uint32_t(callback.source_top))
                : extended_effector_modes() && effector_effects_.contains(callback.display_object)
                ? compose_effector_at(scratch, callback.display_object)
                : sprites_.count(callback.display_object) != 0
                ? compose_single_bitmap_sprite_at(
                      scratch, callback.display_object, x, y)
                : window_objects_.count(callback.display_object) != 0 &&
                      compose_window_at(
                          scratch, callback.display_object, x, y);
        }
        if (composed) {
            const auto& result = graphics_surfaces_.at(scratch);
            for (std::size_t row = 0; row < height; ++row) {
                std::copy_n(
                    result.pixels.begin() +
                        static_cast<std::ptrdiff_t>(row * result.stride),
                    result.stride,
                    target->second.pixels.begin() +
                        static_cast<std::ptrdiff_t>(base + row * target_stride));
            }
            if (filter_rgb555) target->second.rgba.clear();
            else refresh_bgra_mirror(target->second);
        }
        if (!composed && display_failure_serial_ == failure_serial) {
            if (icon_record != nullptr) {
                fail_display_draw(scratch, callback.display_object, "icon",
                    icon_record->animation_angle_q16 != 0
                        ? "icon rotated-frame rendering or bitmap blit failed"
                        : "icon source bitmap is missing or its blit is unsupported",
                    icon_render_bitmap(*icon_group, icon_index));
            } else fail("display object has no implemented pixel callback");
        }
        if (!composed && debug_trace(DebugTrace::Display)) {
            static std::set<std::pair<uint32_t,uint32_t>> reported;
            const uint32_t selected = icon_record ? icon_render_bitmap(*icon_group,icon_index) : 0U;
            if (reported.emplace(callback.display_object,selected).second) {
                std::cerr << "DISPLAY_PIXEL_FAILURE object=" << callback.display_object
                          << " icon=" << (icon_record != nullptr) << " source=" << selected
                          << " target=" << callback.bitmap << " format=" << target->second.format
                          << " size=" << target->second.width << ',' << target->second.height
                          << " view=" << width << ',' << height << " pos=" << x << ',' << y;
                if (const auto src=graphics_surfaces_.find(selected);src!=graphics_surfaces_.end())
                    std::cerr << " source_format=" << src->second.format << " source_size="
                              << src->second.width << ',' << src->second.height << " bpp="
                              << src->second.bytes_per_pixel << " pixels=" << src->second.pixels.size()
                              << " encoded=" << src->second.encoded.size();
                std::cerr << '\n';
            }
        }
        cleanup();
        return composed;
    } catch (...) {
        cleanup();
        throw;
    }
}

}  // namespace bgiplayer
