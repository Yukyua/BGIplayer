#include "bgiplayer/vm.hpp"

namespace bgiplayer {

// Grp0:D0's original wrapper registers the new knob with the mouse manager
// (PE64 1400D83DC -> 1400B7A50), separately from Grp0:DE's wheel list.
void VM::register_control_pointer(uint32_t handle) {
    const auto* control = lookup_control(handle);
    if (!control) return;
    const uint32_t priority = base_display_priority(control->target);
    const auto position = std::find_if(active_display_objects_.begin(), active_display_objects_.end(),
        [priority](const ActiveDisplayNode& node) { return node.priority <= priority; });
    active_display_objects_.insert(position, {handle, priority, false});
    add_display_object_region(control->target);
}

void VM::unregister_control_pointer(uint32_t handle) {
    if (active_display_id_ == handle) active_display_id_ = 0;
    if (const auto* control = lookup_control(handle)) remove_input_regions_for_object(control->target);
    std::erase_if(active_display_objects_, [handle](const ActiveDisplayNode& node) { return node.id == handle; });
}

bool VM::control_pointer_button(int32_t x, int32_t y, bool down) {
    if (!down) {
        if (active_display_id_ == 0) return false;
        enqueue_system_event_record(0x1001U, active_display_id_);
        active_display_id_ = 0;
        return true;
    }
    for (const auto& node : active_display_objects_) {
        auto* control = lookup_control(node.id);
        if (!control || !control->enabled) continue;
        const uint32_t priority = base_display_priority(control->target);
        // Original 1400B7980 queries the registered knob's region through
        // 1400C6400; the knob's virtual rectangle/mask delegates to its target.
        register_display_object_region(control->target, priority);
        if (!input_region_hit_at(priority, x, y)) continue;
        const auto position = object_position(control->target);
        control->grab_x = control->field_150 && position ? x - position->first : 0;
        control->grab_y = control->field_150 && position ? y - position->second : 0;
        active_display_id_ = node.id;
        enqueue_system_event_record(0x1000U, node.id);
        return true;
    }
    return false;
}

void VM::control_pointer_motion(int32_t x, int32_t y) {
    auto* control = lookup_control(active_display_id_);
    if (!control) return;
    if ((input_descriptor(1) & 0x8000U) == 0) {
        (void)control_pointer_button(x, y, false);
        return;
    }
    const auto state = display_object_states_.find(control->target);
    if (state == display_object_states_.end()) return;
    // 14005C950 / legacy 00426570: clamp pixels, round to the nearest
    // discrete division with signed IDIV, then let the ordinary setter
    // reject invalid axes. A one-division endpoint can return changed without
    // storing a new coordinate; do not replace it with a generic clamp.
    const auto axis = [](int32_t mouse, int32_t grab, int32_t base,
                         uint32_t first, uint32_t last, uint32_t size, int32_t divisions) {
        const int32_t maximum = static_cast<int32_t>(last - (size - 1U));
        int32_t pixel = std::min(std::max(mouse - grab - base, static_cast<int32_t>(first)), maximum);
        if (divisions > 0) {
            const auto geometry = control_axis_geometry(first, last, static_cast<int32_t>(size - 1U), divisions);
            const int32_t rounding = divisions > 1 ? maximum / (2 * divisions - 2) : 0;
            const int32_t fixed = static_cast<int32_t>(static_cast<uint32_t>(pixel + rounding) << 16U);
            pixel = fixed / geometry.step_q16;
        }
        return pixel;
    };
    const int32_t next_x = axis(x, control->grab_x, control->absolute_x,
        control->scroll_left_164, control->scroll_right_16c, state->second.width, control->offset_x);
    const int32_t next_y = axis(y, control->grab_y, control->absolute_y,
        control->scroll_top_168, control->scroll_bottom_170, state->second.height, control->offset_y);
    if (next_x == control->interaction_x && next_y == control->interaction_y) return;
    (void)set_control_interaction_position(active_display_id_, next_x, next_y);
    set_display_dirty();
}

} // namespace bgiplayer
