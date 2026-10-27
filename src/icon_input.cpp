#include "bgiplayer/vm.hpp"
#include <iostream>

namespace bgiplayer {

uint32_t VM::icon_input_window_key(const IconInputProcessor& processor) const {
    // PE32 004201E0 / PE64 140056810; use the live window virtual key.
    return base_display_priority(processor.window);
}

void VM::register_icon_processor_input(IconInputProcessor& processor) {
    const auto& words = processor.basic_layout
        ? processor.basic_layout->words : std::array<uint32_t, 8>{};
    const bool configured = processor.basic_layout || !processor.layout.groups.empty();
    if (!configured) return;
    const uint32_t modal = processor.mode == 0 ? words[3] : processor.layout.words[3];
    const uint32_t parent_region = processor.mode == 0 ? words[4] : processor.layout.words[4];
    const bool trace = debug_trace(DebugTrace::IconInput);
    if (trace) std::cout << "ICON_INPUT_LAYOUT processor=" << processor.id
        << " mode=" << processor.mode << " window_key=" << icon_input_window_key(processor)
        << " modal=" << modal << " parent_region=" << parent_region
        << " auto_select=" << (processor.mode == 0 ? words[7] : processor.layout.words[7]) << '\n';
    if (modal != 0) {
        const auto key = icon_input_window_key(processor);
        arm_input_capture(key);
        icon_input_capture_owners_[processor.id] = key;
    }
    else if (parent_region != 0) add_display_object_region(processor.window);
}

void VM::release_icon_processor_input(const IconInputProcessor& processor) {
    const bool configured = processor.basic_layout || !processor.layout.groups.empty();
    if (!configured) return;
    const uint32_t modal = processor.mode == 0
        ? processor.basic_layout->words[3] : processor.layout.words[3];
    const uint32_t parent_region = processor.mode == 0
        ? processor.basic_layout->words[4] : processor.layout.words[4];
    if (modal != 0) {
        disarm_input_capture(icon_input_window_key(processor));
        icon_input_capture_owners_.erase(processor.id);
    }
    else if (parent_region != 0) remove_input_region(processor.window);
}

bool VM::icon_input_priority_hit(const IconInputProcessor& processor, int32_t x, int32_t y) const {
    // 004511A0 / 14008F2C0: capture gate first, then the child's global region.
    const auto key = icon_input_window_key(processor);
    // This is the owner-capture gate (00479DC0 / 1400C6330), not the
    // keyboard gate 00478E90, which also consults Sys0:10's global flag.
    for (const auto& [owner, priority] : icon_input_capture_owners_)
        if (priority > key) return false;
    return input_region_hit_at(key + 0x8000U, x, y);
}

bool VM::input_object_mask_hit(uint32_t object, int32_t x, int32_t y,
                                uint32_t width, uint32_t height) const {
    if (x < 0 || y < 0 || static_cast<uint32_t>(x) >= width ||
        static_cast<uint32_t>(y) >= height) return false;
    // Extended virtual children transform the point into their bitmap space
    // before applying the independent word12 mask, then test the parent.
    for (const auto& [id, processor] : icon_input_processors_) {
        (void)id;
        for (const auto& group : processor.layout.groups)
            for (std::size_t index = 0; index < group.icons.size(); ++index) {
                const auto& icon = group.icons[index];
                if (icon.display_object_id != object) continue;
                const auto bounds = icon_render_bounds(group, index);
                const double dx = x + bounds.left - icon.render_x;
                const double dy = y + bounds.top - icon.render_y;
                const double angle = icon_render_angle_degrees(icon) * std::numbers::pi / 180.0;
                const auto bitmap = graphics_surfaces_.find(icon_render_bitmap(group,index));
                if (bitmap == graphics_surfaces_.end()) return false;
                const auto pixel = icon_source_pixel(dx*std::cos(angle)+dy*std::sin(angle),
                    -dx*std::sin(angle)+dy*std::cos(angle), bitmap->second.width,bitmap->second.height);
                if (!pixel) return false;
                const auto mask = object_masks_.find(object);
                if (mask != object_masks_.end()) {
                    const auto& m=mask->second;
                    const auto offset=static_cast<std::size_t>(pixel->second)*m.stride+(pixel->first>>3U);
                    if(pixel->first>=m.width || pixel->second>=m.height || offset>=m.bits.size() ||
                       (m.bits[offset]&(1U<<(pixel->first&7U)))==0) return false;
                }
                const auto parent=display_object_states_.find(processor.window);
                return parent != display_object_states_.end() && input_object_mask_hit(processor.window,
                    x+bounds.left,y+bounds.top,parent->second.width,parent->second.height);
            }
    }
    // 00420F80 / 140055890. A missing mask is an enabled rectangular hit,
    // not the current frame's opacity. Explicit masks are cached one-bit maps.
    const auto mask = object_masks_.find(object);
    if (mask != object_masks_.end()) {
        const auto& state = mask->second;
        if (static_cast<uint32_t>(x) >= state.width || static_cast<uint32_t>(y) >= state.height)
            return false;
        const auto offset = static_cast<std::size_t>(y) * state.stride +
                            (static_cast<uint32_t>(x) >> 3U);
        if (offset >= state.bits.size() ||
            (state.bits[offset] & (1U << (static_cast<uint32_t>(x) & 7U))) == 0) return false;
    }
    if (window_objects_.count(object) != 0) return true;
    // Child virtual 0042FCD0 / 140067160 also checks its parent's mask.
    for (const auto& [id, processor] : icon_input_processors_) {
        const auto display = display_object_states_.find(processor.window);
        if (display == display_object_states_.end()) continue;
        const auto child = display_object_states_.find(object);
        if (child == display_object_states_.end()) continue;
        const auto parent_hit = [&] {
            return input_object_mask_hit(processor.window,
                x + child->second.x - display->second.x,
                y + child->second.y - display->second.y,
                display->second.width, display->second.height);
        };
        for (const auto& group : processor.layout.groups)
            for (const auto& icon : group.icons)
                if (icon.display_object_id == object) return parent_hit();
        if (processor.basic_layout)
            for (const auto& group : processor.basic_layout->groups)
                for (const auto& icon : group.icons)
                    if (icon.display_object_id == object) return parent_hit();
    }
    return true;
}

bool VM::queue_icon_input_at(int32_t x, int32_t y, bool pressed) {
    bool generated = false;
    // Refresh every processor before deciding which window owns the input.
    // Otherwise map iteration can test a lower window against stale regions.
    for (const auto& [id, processor] : icon_input_processors_) {
        refresh_icon_input_regions(id);
        refresh_basic_icon_input_regions(id);
    }
    update_extended_icon_hover_at(x, y);
    for (auto& [id, processor] : icon_input_processors_) {
        if (pressed && (generated || !icon_input_priority_hit(processor, x, y))) continue;
        if (pressed && processor.mode == 0 && processor.state[0] == 0) {
            continue;
        }
        const auto window =
            window_objects_.find(processor.window);
        const int32_t window_x =
            window == window_objects_.end()
                ? 0
                : window->second.x;
        const int32_t window_y =
            window == window_objects_.end()
                ? 0
                : window->second.y;
        if (!pressed) {
            if (processor.mode != 0 && processor.pending_icon) {
                const auto pending = *processor.pending_icon;
                processor.pending_icon.reset();
                if (extended_icon_input_hit_at(processor, x, y) == pending) {
                    accept_extended_icon_input(processor, pending, x, y);
                    generated = true;
                }
            }
            if (processor.pressed_icon) {
                processor.pressed_icon.reset();
                generated = true;
            }
            // Mouse-up is not a 10000006(state=0) action. The original only
            // emits that notification for a separately matched shortcut.
            continue;
        }
        processor.pending_icon.reset();
        const auto hit = extended_icon_input_hit_at(processor,x,y);
        const std::optional<std::size_t> best_group = hit
            ? std::optional<std::size_t>(*hit >> 16U) : std::nullopt;
        const std::optional<std::size_t> best_icon = hit
            ? std::optional<std::size_t>(*hit & 0xFFFFU) : std::nullopt;
        if (best_group && best_icon) {
            const uint32_t packed = *hit;
            const auto& group = processor.layout.groups[*best_group];
            const auto& icon = group.icons[*best_icon];
            // 00453E30 / 1400919F0: a selected repeat-disabled item cannot
            // accept the same action again.
            if (icon.words[13] != 0 && group.words[3] == *best_icon) continue;
            processor.pressed_icon = packed;
            if ((group.words[15] & 2U) != 0 || (icon.words[48] & 0x20U) != 0)
                processor.pending_icon = packed;
            else accept_extended_icon_input(processor, packed, x, y);
            generated = true;
        }
        if (processor.mode == 0 &&
            processor.basic_layout &&
            !processor.pressed_icon) {
            std::optional<std::size_t> basic_group;
            std::optional<std::size_t> basic_icon;
            uint32_t basic_priority = 0;
            uint32_t basic_sequence = 0;
            for (std::size_t group = 0;
                 group < processor.basic_layout->groups.size();
                 ++group) {
                const BasicIconInputGroupRecord& record =
                    processor.basic_layout->groups[group];
                for (std::size_t icon = 0;
                     icon < record.icons.size(); ++icon) {
                    const BasicIconInputIconRecord& candidate =
                        record.icons[icon];
                    const uint32_t priority =
                        0x8000U + basic_sequence++;
                    if (candidate.words[0] == 0) {
                        continue;
                    }
                    uint32_t bitmap = candidate.words[3];
                    if (icon == record.words[2] &&
                        static_cast<int32_t>(candidate.words[4]) !=
                            -1) {
                        bitmap = candidate.words[4];
                    }
                    auto surface = graphics_surfaces_.find(bitmap);
                    if (surface == graphics_surfaces_.end()) {
                        continue;
                    }
                    const int32_t left = candidate.render_x;
                    const int32_t top = candidate.render_y;
                    const int32_t absolute_left = window_x + left;
                    const int32_t absolute_top = window_y + top;
                    const int32_t right =
                        absolute_left +
                        static_cast<int32_t>(surface->second.width);
                    const int32_t bottom =
                        absolute_top +
                        static_cast<int32_t>(surface->second.height);
                    if (x < absolute_left || x >= right ||
                        y < absolute_top || y >= bottom) {
                        continue;
                    }
                    // 004511A0 -> child virtual+64 -> 0042FCD0/00420F80:
                    // no explicit mask means a rectangular hit, including
                    // transparent padding and spaces between button letters.
                    if (!input_object_mask_hit(candidate.display_object_id,
                            x - absolute_left, y - absolute_top,
                            surface->second.width, surface->second.height)) {
                        continue;
                    }
                    if (!basic_group ||
                        priority >= basic_priority) {
                        basic_group = group;
                        basic_icon = icon;
                        basic_priority = priority;
                    }
                }
            }
            if (basic_group && basic_icon) {
                const uint32_t packed =
                    (static_cast<uint32_t>(*basic_group) << 16U) |
                    static_cast<uint32_t>(*basic_icon);
                const BasicIconInputIconRecord& selected =
                    processor.basic_layout->groups[*basic_group]
                        .icons[*basic_icon];
                // Basic vtable 00510CFC: action1 in 004501A0 stores
                // the accepted flat hit through 00451B30, then its +14
                // returns1. 004500D0 consequently clears active(+30).
                processor.selected_group = static_cast<int32_t>(*basic_group);
                processor.selected_icon = static_cast<int32_t>(*basic_icon);
                processor.selected_state = 1;
                processor.state[0] = 0;
                processor.pressed_icon = packed;
                processor.state[1] = *basic_group;
                processor.state[2] = *basic_icon;
                processor.state[3] = 1;
                processor.state[4] = static_cast<uint32_t>(
                    x - window_x - selected.render_x);
                processor.state[5] = static_cast<uint32_t>(
                    y - window_y - selected.render_y);
                processor.events.push_back(
                    {0x10000002U, packed,
                     selected.words[5] != 0xFFFFFFFFU ? 1U : 0U});
                generated = true;
            }
        }
    }
    return generated;
}

void VM::accept_extended_icon_input(IconInputProcessor& processor, uint32_t packed,
                                    int32_t x, int32_t y) {
    const auto group = packed >> 16U;
    const auto index = packed & 0xFFFFU;
    const auto& icon = processor.layout.groups[group].icons[index];
    // Recheck on synthesized release actions as well: BP may have changed
    // the selected index while this physical press was pending.
    if (icon.words[13] != 0 && processor.layout.groups[group].words[3] == index) return;
    const auto window = window_objects_.find(processor.window);
    const auto window_x = window == window_objects_.end() ? 0 : window->second.x;
    const auto window_y = window == window_objects_.end() ? 0 : window->second.y;
    processor.selected_group = static_cast<int32_t>(group);
    processor.selected_icon = static_cast<int32_t>(index);
    processor.selected_state = 1;
    processor.state[1] = group;
    processor.state[2] = index;
    processor.state[3] = 1;
    processor.state[4] = static_cast<uint32_t>(x - window_x - icon.render_x);
    processor.state[5] = static_cast<uint32_t>(y - window_y - icon.render_y);
    processor.events.push_back({0x10000007U, packed,
        (processor.state[5] << 16U) | (processor.state[4] & 0xFFFFU)});
    processor.events.push_back({0x10000006U, packed, 1});
    if (processor.layout.words[7] != 0) {
        const std::array<uint32_t, 3> select{0x10000002U, group, index};
        (void)dispatch_icon_input_property_array(processor, select);
    }
}

} // namespace bgiplayer
