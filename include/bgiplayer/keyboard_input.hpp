#pragma once

#include <SDL_events.h>

#include <cstdint>
#include <array>
#include <map>
#include <span>
#include <set>

namespace bgiplayer {
class VM;
enum class HostKeyboardLayout { Logical, Us, French, German, Japanese };
HostKeyboardLayout host_keyboard_layout();
uint16_t host_keyboard_modifiers(uint16_t fallback);
uint32_t windows_virtual_key_from_sdl_layout(uint32_t keycode, uint32_t scancode,
                                            HostKeyboardLayout layout);

// Host-only physical bindings. These are rebuilt from SDL, never restored from
// a VM snapshot. Engine press latches and queued events remain owned by VM.
class SdlKeyboardInput {
public:
    enum class Action { None, Close, ToggleFullscreen };
    Action handle_event(VM& vm, const SDL_Event& event);
    // SDL's translated keycode loses layout-specific Windows VK values and
    // AltGr's synthetic Ctrl. Feed the original parent-window messages on Win32.
    void use_windows_messages(bool enabled) { windows_messages_ = enabled; }
    void set_layout(HostKeyboardLayout layout) { layout_ = layout; }
    Action handle_windows_message(VM& vm, uint32_t message, uint32_t virtual_key,
                                  uint32_t flags);
    static void flush_window_switch(VM& vm, bool& requested,
                                    bool host_modal = false,
                                    bool renderer_ready = true);
    void release_all(VM& vm);
    void synchronize(VM& vm, std::span<const uint8_t> scancodes);
    void sample_raw_held(VM& vm, std::span<const uint8_t> scancodes);
    [[nodiscard]] std::array<uint8_t, 256> keyboard_state(
        const VM& vm, uint16_t modifiers) const;

private:
    struct Binding {
        uint32_t descriptor = 0;
        uint32_t side_descriptor = 0;
    };
    void update_raw_state(VM& vm) const;
    std::map<uint32_t, Binding> held_;
    bool focused_ = true;
    bool windows_messages_ = false;
    HostKeyboardLayout layout_ = HostKeyboardLayout::Logical;
    std::set<std::pair<uint32_t, uint32_t>> warned_unmapped_;
};
}  // namespace bgiplayer
