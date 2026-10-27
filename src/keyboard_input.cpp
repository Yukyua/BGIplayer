#include "bgiplayer/keyboard_input.hpp"

#include "bgiplayer/application.hpp"
#include "bgiplayer/vm.hpp"

#include <SDL_keyboard.h>

#include <array>
#include <algorithm>
#include <utility>
#include <iostream>

namespace bgiplayer {
#ifndef __APPLE__
HostKeyboardLayout host_keyboard_layout() { return HostKeyboardLayout::Logical; }
uint16_t host_keyboard_modifiers(uint16_t fallback) { return fallback; }
#endif

uint32_t windows_virtual_key_from_sdl_layout(uint32_t keycode, uint32_t scancode,
                                            HostKeyboardLayout layout) {
    // Microsoft layout sample overrides of the standard Set-1 scan->VK table.
    // SDL scancodes identify the same physical keys on ANSI/ISO/JIS hardware.
    const auto scan = scancode == SDL_SCANCODE_NONUSHASH ? SDL_SCANCODE_BACKSLASH :
        static_cast<SDL_Scancode>(scancode);
    if (layout == HostKeyboardLayout::German) {
        switch (scan) {
            case SDL_SCANCODE_GRAVE: return 0xDC;
            case SDL_SCANCODE_MINUS: return 0xDB;
            case SDL_SCANCODE_EQUALS: return 0xDD;
            case SDL_SCANCODE_Y: return 'Z';
            case SDL_SCANCODE_Z: return 'Y';
            case SDL_SCANCODE_LEFTBRACKET: return 0xBA;
            case SDL_SCANCODE_RIGHTBRACKET: return 0xBB;
            case SDL_SCANCODE_BACKSLASH: return 0xBF;
            case SDL_SCANCODE_SEMICOLON: return 0xC0;
            case SDL_SCANCODE_SLASH: return 0xBD;
            default: break;
        }
    } else if (layout == HostKeyboardLayout::French) {
        switch (scan) {
            case SDL_SCANCODE_GRAVE: return 0xDE;
            case SDL_SCANCODE_MINUS: return 0xDB;
            case SDL_SCANCODE_Q: return 'A';
            case SDL_SCANCODE_W: return 'Z';
            case SDL_SCANCODE_A: return 'Q';
            case SDL_SCANCODE_Z: return 'W';
            case SDL_SCANCODE_LEFTBRACKET: return 0xDD;
            case SDL_SCANCODE_RIGHTBRACKET: return 0xBA;
            case SDL_SCANCODE_SEMICOLON: return 'M';
            case SDL_SCANCODE_APOSTROPHE: return 0xC0;
            case SDL_SCANCODE_M: return 0xBC;
            case SDL_SCANCODE_COMMA: return 0xBE;
            case SDL_SCANCODE_PERIOD: return 0xBF;
            case SDL_SCANCODE_SLASH: return 0xDF;
            default: break;
        }
    } else if (layout == HostKeyboardLayout::Japanese) {
        switch (scan) {
            case SDL_SCANCODE_EQUALS: return 0xDE;
            case SDL_SCANCODE_LEFTBRACKET: return 0xC0;
            case SDL_SCANCODE_RIGHTBRACKET: return 0xDB;
            case SDL_SCANCODE_BACKSLASH: return 0xDD;
            case SDL_SCANCODE_SEMICOLON: return 0xBB;
            case SDL_SCANCODE_APOSTROPHE: return 0xBA;
            case SDL_SCANCODE_GRAVE: return 0x19;
            case SDL_SCANCODE_LANG2: return 0xF0; // macOS Eisu -> DBE_ALPHANUMERIC.
            default: break;
        }
    }
    return windows_virtual_key_from_sdl(keycode, scancode);
}
namespace {
uint32_t physical_key(const SDL_Keysym& keysym) {
    return keysym.scancode != SDL_SCANCODE_UNKNOWN
        ? static_cast<uint32_t>(keysym.scancode)
        : 0x80000000U | static_cast<uint32_t>(keysym.sym);
}
uint32_t modifier_side(SDL_Keycode key) {
    switch (key) {
        case SDLK_LSHIFT: return 0xA0;
        case SDLK_RSHIFT: return 0xA1;
        case SDLK_LCTRL: return 0xA2;
        case SDLK_RCTRL: return 0xA3;
        case SDLK_LALT: return 0xA4;
        case SDLK_RALT: return 0xA5;
        default: return 0;
    }
}
bool keyboard_descriptor(uint32_t descriptor) {
    // 0E/0F are the original engine's synthetic wheel descriptors.
    return descriptor >= 8 && descriptor != 14 && descriptor != 15;
}
bool window_mode_key(const VM& vm, uint32_t descriptor) {
    // 0046BDB0 / 1400B6140 consult Sys0:62's enabled descriptor list.
    const auto& keys = vm.active_descriptors();
    return vm.active_descriptor_value() != 0 &&
        std::find(keys.begin(), keys.end(), descriptor) != keys.end();
}
}  // namespace

std::array<uint8_t, 256> SdlKeyboardInput::keyboard_state(
    const VM& vm, uint16_t modifiers) const {
    std::array<uint8_t, 256> states{};
    for (uint32_t key = 0; key < states.size(); ++key) {
        if ((vm.input_descriptor(key) & 0x8000U) != 0) states[key] = 0x80;
    }
    // GetKeyboardState's low bit is a lock toggle, never GetAsyncKeyState's
    // low "pressed since query" bit. SDL exposes the same lock state.
    if ((modifiers & KMOD_CAPS) != 0) states[0x14] |= 1;
    if ((modifiers & KMOD_NUM) != 0) states[0x90] |= 1;
    if ((modifiers & KMOD_SCROLL) != 0) states[0x91] |= 1;
    return states;
}

void SdlKeyboardInput::flush_window_switch(VM& vm, bool& requested,
                                          bool host_modal, bool renderer_ready) {
    const bool pending = std::exchange(requested, false);
    // The original flag coalesces multiple requests before the next host
    // processing pass: 0046BA9E / PE64 1400B652A.
    if (!pending || vm.active_descriptor_value() == 0) return;
    const auto& movie = vm.global_movie();
    // 0049AFE0 / 1400F2550 is the global movie's active flag, cleared at
    // completion/teardown. A paused but incomplete movie still blocks.
    const bool movie_active = movie && movie->media_open && movie->started &&
        (movie->playing || movie->position_ms < movie->duration_ms);
    // Original 0046BA9E / 1400B652A rejects the DirectShow movie flag,
    // started Media Foundation playback and an existing EDIT (even hidden).
    if (movie_active || vm.mf_movie_active() ||
        vm.host_text_edit().is_open() || host_modal || !renderer_ready) return;
    if (vm.select_window_mode(vm.window_fullscreen() == 0 ? 1U : 0U,
                              vm.window_mode_flag(), vm.window_mode_index())) {
        vm.enqueue_system_event_record(1, vm.window_fullscreen());
    }
}

void SdlKeyboardInput::update_raw_state(VM& vm) const {
    std::array<bool, 256> down{};
    for (const auto& [physical, binding] : held_) {
        (void)physical;
        down[binding.descriptor] = true;
        if (binding.side_descriptor) down[binding.side_descriptor] = true;
    }
    for (uint32_t key = 8; key < down.size(); ++key) {
        if (keyboard_descriptor(key)) {
            vm.set_input_descriptor_raw_state(key, down[key] ? 0x8000U : 0U);
        }
    }
}

void SdlKeyboardInput::release_all(VM& vm) {
    held_.clear();
    for (uint32_t key = 8; key < 256; ++key) {
        if (keyboard_descriptor(key)) vm.notify_keyboard_key_up(key);
    }
    update_raw_state(vm);
}

void SdlKeyboardInput::synchronize(VM& vm,
                                  std::span<const uint8_t> scancodes) {
    // SDL cannot deliver releases that happened while unfocused (or before a
    // debug snapshot was loaded). Rebuild held state without inventing presses.
    release_all(vm);
    sample_raw_held(vm, scancodes);
    focused_ = true;
}

void SdlKeyboardInput::sample_raw_held(VM& vm,
                                     std::span<const uint8_t> scancodes) {
    // Native child EDIT/NSTextView receives typing without a parent key
    // message. Update the processed held table without changing BP latches.
    for (auto iterator = held_.begin(); iterator != held_.end();) {
        if (iterator->first < scancodes.size() && scancodes[iterator->first] == 0)
            iterator = held_.erase(iterator);
        else ++iterator;
    }
    for (std::size_t index = 1; index < scancodes.size(); ++index) {
        if (scancodes[index] == 0) continue;
        if (held_.contains(static_cast<uint32_t>(index))) continue;
        const auto scan = static_cast<SDL_Scancode>(index);
        const SDL_Keycode key = SDL_GetKeyFromScancode(scan);
        const uint32_t descriptor = windows_virtual_key_from_sdl_layout(
            static_cast<uint32_t>(key), static_cast<uint32_t>(index), layout_);
        if (descriptor != 0) held_[static_cast<uint32_t>(index)] =
            {descriptor, modifier_side(key)};
    }
    update_raw_state(vm);
}

SdlKeyboardInput::Action SdlKeyboardInput::handle_event(
    VM& vm, const SDL_Event& event) {
    if (event.type == SDL_WINDOWEVENT) {
        // Original WM_SIZE: SIZE_RESTORED enables held-key input/Alt+Return;
        // SIZE_MINIMIZED disables it. This is not a script skip-policy flag.
        if (event.window.event == SDL_WINDOWEVENT_MINIMIZED)
            vm.set_input_return_gate(false);
        else if (event.window.event == SDL_WINDOWEVENT_RESTORED)
            vm.set_input_return_gate(true);
    }
    if (event.type == SDL_WINDOWEVENT &&
        event.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) {
        // WM_ACTIVATE/WM_ACTIVATEAPP update the Sys0:0F foreground getter.
        vm.set_sys0_state_542c58(1);
        focused_ = true;
    }
    if (event.type == SDL_WINDOWEVENT &&
        event.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
        vm.set_sys0_state_542c58(0);
        release_all(vm);
        focused_ = false;
        return Action::None;
    }
    if (event.type != SDL_KEYDOWN && event.type != SDL_KEYUP) {
        return Action::None;
    }
    if (!focused_) return Action::None;
    if (windows_messages_) return Action::None;

    const bool down = event.type == SDL_KEYDOWN;
    const auto& key = event.key.keysym;
    const uint32_t physical = physical_key(key);
    auto found = held_.find(physical);
    Binding binding;
    if (found != held_.end()) {
        binding = found->second;
    } else {
        binding = {windows_virtual_key_from_sdl_layout(
            static_cast<uint32_t>(key.sym), static_cast<uint32_t>(key.scancode), layout_),
            modifier_side(key.sym)};
    }
    if (binding.descriptor == 0) {
        if (down && warned_unmapped_.insert({static_cast<uint32_t>(key.sym),
                                           static_cast<uint32_t>(key.scancode)}).second)
            std::cerr << "warning: KEYBOARD_KEY_UNMAPPED keycode="
                      << static_cast<uint32_t>(key.sym) << " scancode="
                      << static_cast<uint32_t>(key.scancode)
                      << " reason=no-verified-Windows-VK-category\n";
        return Action::None;
    }
    if (down && layout_ == HostKeyboardLayout::Logical &&
        static_cast<uint32_t>(key.sym) > 0x7F &&
        static_cast<uint32_t>(key.sym) <= 0x10FFFF &&
        binding.descriptor >= 0xBA && binding.descriptor <= 0xE2 &&
        warned_unmapped_.insert({static_cast<uint32_t>(key.sym),
                                 static_cast<uint32_t>(key.scancode)}).second)
        std::cerr << "warning: KEYBOARD_LAYOUT_FALLBACK keycode="
                  << static_cast<uint32_t>(key.sym) << " scancode="
                  << static_cast<uint32_t>(key.scancode) << " vk="
                  << binding.descriptor
                  << " reason=host-layout-has-no-verified-OEM-profile\n";
    if (down) held_[physical] = binding;
    else if (found != held_.end()) held_.erase(found);
    update_raw_state(vm);

    const uint32_t descriptor = binding.descriptor;
    const bool system_key = descriptor == 0x12 || descriptor == 0x79 ||
        (key.mod & KMOD_ALT) != 0;
    if (system_key) {
        // WM_SYSKEYDOWN/UP: Alt itself is posted on release, F10 on down.
        // Other Alt combinations are owned by the host window/system, not the
        // ordinary BP key queue (004A6730 / 140101938..140101A29).
        if (!down) {
            const bool toggle = descriptor == 0x12 &&
                vm.notify_keyboard_key_down(descriptor, false) &&
                window_mode_key(vm, descriptor);
            if (descriptor == 0x12 || descriptor == 0x79)
                vm.notify_keyboard_key_up(descriptor);
            if (toggle) return Action::ToggleFullscreen;
        } else if (descriptor == 0x79) {
            if (vm.notify_keyboard_key_down(descriptor, false) &&
                window_mode_key(vm, descriptor)) return Action::ToggleFullscreen;
        } else if (descriptor == 0x73) {
            return Action::Close;
        } else if (descriptor == 0x0D && event.key.repeat == 0 &&
                   vm.input_return_gate()) {
            return Action::ToggleFullscreen;
        }
        return Action::None;
    }
    if (down) {
        if (vm.notify_keyboard_key_down(descriptor) &&
            window_mode_key(vm, descriptor)) return Action::ToggleFullscreen;
    }
    else vm.notify_keyboard_key_up(descriptor);
    return Action::None;
}

SdlKeyboardInput::Action SdlKeyboardInput::handle_windows_message(
    VM& vm, uint32_t message, uint32_t key, uint32_t flags) {
    // WM_KEYDOWN/UP and WM_SYSKEYDOWN/UP, preserved before SDL's IME/AltGr
    // normalization. Actual virtual-key values are supplied by the active HKL.
    if (!focused_ || key >= 256 || message < 0x100 || message > 0x105)
        return Action::None;
    if (message == 0x100) {
        if (vm.notify_keyboard_key_down(key) && window_mode_key(vm, key))
            return Action::ToggleFullscreen;
    } else if (message == 0x101) {
        vm.notify_keyboard_key_up(key);
    } else if (message == 0x104) {
        // These paths call the latch core directly, without WM_KEYDOWN's
        // configured repeat rearm (004A6730 / 140101938..140101A29).
        if (key == 0x79) {
            if (vm.notify_keyboard_key_down(key, false) && window_mode_key(vm, key))
                return Action::ToggleFullscreen;
        } else if (key == 0x73) {
            return Action::Close;
        } else if (key == 0x0D && (flags & (1U << 30U)) == 0 &&
                   vm.input_return_gate()) {
            return Action::ToggleFullscreen;
        }
    } else if (message == 0x105 && (key == 0x12 || key == 0x79)) {
        const bool toggle = key == 0x12 &&
            vm.notify_keyboard_key_down(key, false) && window_mode_key(vm, key);
        vm.notify_keyboard_key_up(key);
        if (toggle) return Action::ToggleFullscreen;
    }
    return Action::None;
}
}  // namespace bgiplayer
