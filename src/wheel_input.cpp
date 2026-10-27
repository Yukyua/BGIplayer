#include "bgiplayer/wheel_input.hpp"

#include "bgiplayer/vm.hpp"

#include <cmath>

namespace bgiplayer {
namespace {
double wheel_axis(int integral, float precise, uint32_t direction) {
    if (!std::isfinite(precise)) return 0.0;
    double delta = precise != 0.0F ? precise : static_cast<double>(integral);
    if (direction == SDL_MOUSEWHEEL_FLIPPED) delta = -delta;
    return delta;
}
}  // namespace

bool handle_sdl_mouse_wheel(VM& vm, const SDL_MouseWheelEvent& event) {
    float precise_x = 0.0F, precise_y = 0.0F;
#if SDL_VERSION_ATLEAST(2, 0, 18)
    // SDL preserves fractional Win32 deltas and Cocoa trackpad movement here;
    // the integral y can be zero, or include an earlier accumulated fraction.
    precise_x = event.preciseX;
    precise_y = event.preciseY;
#endif
    // Cocoa tags natural/inverted device directions. Normalize once to the
    // original Win32 detent orientation; do not add an OS-specific second flip.
    const double vertical = wheel_axis(event.y, precise_y, event.direction);
    const double horizontal = wheel_axis(event.x, precise_x, event.direction);
    // Windows sends separate messages. Cocoa can combine axes; splitting its
    // event in vertical-then-horizontal order is an explicit host policy.
    if (vertical != 0.0) vm.notify_mouse_wheel(vertical < 0.0);
    if (horizontal != 0.0) vm.notify_mouse_horizontal_wheel(horizontal > 0.0);
    return vertical != 0.0 || horizontal != 0.0;
}

void VM::notify_mouse_horizontal_wheel(bool right) {
    const uint32_t descriptor = right ? 0x8FU : 0x8EU;
    // 004A7271 / 140101D60: this path has no control-wheel gate.
    ++sys0_counter_542e90_;
    enqueue_system_event_record(3, descriptor);
    if (!input_descriptor_active_[descriptor])
        queue_input_descriptor_event(descriptor, 1);
    notify_keyboard_key_up(descriptor);
}

void VM::notify_mouse_wheel(bool down) {
    const uint32_t descriptor = down ? 0x0FU : 0x0EU;
    // 004A71E3 / 140101CEA: epoch and FIFO always precede the control gate.
    ++sys0_counter_542e90_;
    enqueue_system_event_record(3, descriptor);

    if (grp0_render_global_529c3c_ != 0 && !active_control_candidates_.empty()) {
        // 0046E450 / 1400B7740 uses the first registered control, without
        // pointer-region testing, enabled checks, or keyboard capture gating.
        const uint32_t id = active_control_candidates_.front();
        const auto slot = std::find_if(control_objects_.begin(), control_objects_.end(),
            [id](const auto& entry) { return entry && entry->id == id; });
        if (slot == control_objects_.end())
            throw BpFault("mouse wheel active control no longer exists");
        auto& control = **slot;
        const int32_t step = down ? 1 : -1;
        const int32_t y = static_cast<int32_t>(
            static_cast<uint32_t>(control.interaction_y) + static_cast<uint32_t>(step));
        const bool moved = set_control_interaction_position(
            0xF0000000U | control.slot, control.interaction_x, y);
        // Original 00426790 / 14005C7F0 records attempted motion even at limits.
        control.query_174 = 1;
        control.query_178 = 0;
        control.query_17c = static_cast<uint32_t>(step);
        control.query_180 = moved ? 0U : 1U;
        set_display_dirty(); // 0046C450 / 1400B6EC0 requests presentation.
        return;
    }

    // The original register helper does not consult Sys1:10 on this path.
    // It only adds a count if the descriptor was unlatched; release retains it.
    if (!input_descriptor_active_[descriptor])
        queue_input_descriptor_event(descriptor, 1);
    notify_keyboard_key_up(descriptor);
}
}  // namespace bgiplayer
