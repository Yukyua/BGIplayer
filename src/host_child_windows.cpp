#include "bgiplayer/host_child_windows.hpp"

#include <utility>

namespace bgiplayer {

HostChildWindows::Window* HostChildWindows::lookup(uint32_t handle) {
    if ((handle & 0xFF000000U) != kHandleTag) {
        return nullptr;
    }
    const uint32_t index = handle & 0x00FFFFFFU;
    if (index >= windows_.size() || !windows_[index].active) {
        return nullptr;
    }
    return &windows_[index];
}

const HostChildWindows::Window* HostChildWindows::lookup(
    uint32_t handle) const {
    if ((handle & 0xFF000000U) != kHandleTag) {
        return nullptr;
    }
    const uint32_t index = handle & 0x00FFFFFFU;
    if (index >= windows_.size() || !windows_[index].active) {
        return nullptr;
    }
    return &windows_[index];
}

uint32_t HostChildWindows::create(
    std::string title, int32_t x, int32_t y, uint32_t width, uint32_t height,
    uint32_t& handle) {
    if (width - 0x20U >= 0x7E1U || height - 0x20U >= 0x7E1U) {
        return kInvalidBounds;
    }
    for (std::size_t index = 0; index < windows_.size(); ++index) {
        if (windows_[index].active) {
            continue;
        }
        Window& window = windows_[index];
        window = Window{};
        window.active = true;
        window.title = std::move(title);
        window.x = x;
        window.y = y;
        window.width = width;
        window.height = height;
        handle = kHandleTag | static_cast<uint32_t>(index);
        return 0;
    }
    return kNoFreeSlot;
}

bool HostChildWindows::destroy(uint32_t handle) {
    Window* window = lookup(handle);
    if (window == nullptr) {
        return false;
    }
    *window = Window{};
    return true;
}

bool HostChildWindows::set_visible(uint32_t handle, uint32_t visible) {
    Window* window = lookup(handle);
    if (window == nullptr) {
        return false;
    }
    window->visible = visible != 0;
    return true;
}

bool HostChildWindows::set_title(uint32_t handle, std::string title) {
    Window* window = lookup(handle);
    if (window == nullptr) {
        return false;
    }
    window->title = std::move(title);
    return true;
}

bool HostChildWindows::set_parameter(uint32_t handle, std::string parameter) {
    Window* window = lookup(handle);
    if (window == nullptr) {
        return false;
    }
    window->parameter = std::move(parameter);
    return true;
}

bool HostChildWindows::set_position(uint32_t handle, int32_t x, int32_t y) {
    Window* window = lookup(handle);
    if (window == nullptr) {
        return false;
    }
    window->x = x;
    window->y = y;
    return true;
}

std::optional<std::pair<int32_t, int32_t>> HostChildWindows::position(
    uint32_t handle) const {
    const Window* window = lookup(handle);
    if (window == nullptr) {
        return std::nullopt;
    }
    return std::pair<int32_t, int32_t>{window->x, window->y};
}

std::optional<std::pair<uint32_t, uint32_t>> HostChildWindows::size(
    uint32_t handle) const {
    const Window* window = lookup(handle);
    if (window == nullptr) {
        return std::nullopt;
    }
    return std::pair<uint32_t, uint32_t>{window->width, window->height};
}

bool HostChildWindows::refresh(uint32_t handle) {
    Window* window = lookup(handle);
    if (window == nullptr) {
        return false;
    }
    window->dirty = true;
    return true;
}

std::optional<bool> HostChildWindows::visible(uint32_t handle) const {
    const Window* window = lookup(handle);
    return window == nullptr ? std::nullopt
                             : std::optional<bool>(window->visible);
}

std::optional<std::string> HostChildWindows::title(uint32_t handle) const {
    const Window* window = lookup(handle);
    return window == nullptr ? std::nullopt
                             : std::optional<std::string>(window->title);
}

std::optional<std::string> HostChildWindows::parameter(
    uint32_t handle) const {
    const Window* window = lookup(handle);
    return window == nullptr ? std::nullopt
                             : std::optional<std::string>(window->parameter);
}

std::optional<bool> HostChildWindows::dirty(uint32_t handle) const {
    const Window* window = lookup(handle);
    return window == nullptr ? std::nullopt
                             : std::optional<bool>(window->dirty);
}

}  // namespace bgiplayer
