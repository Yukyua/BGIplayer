#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace bgiplayer {

class HostChildWindows {
    friend struct SnapshotCodec;
public:
    static constexpr uint32_t kInvalidBounds = 0x80000001U;
    static constexpr uint32_t kNoFreeSlot = 0x80000002U;
    static constexpr uint32_t kHandleTag = 0xFF000000U;

    uint32_t create(std::string title, int32_t x, int32_t y, uint32_t width,
                    uint32_t height, uint32_t& handle);
    bool destroy(uint32_t handle);
    bool set_visible(uint32_t handle, uint32_t visible);
    bool set_title(uint32_t handle, std::string title);
    bool set_parameter(uint32_t handle, std::string parameter);
    bool set_position(uint32_t handle, int32_t x, int32_t y);
    std::optional<std::pair<int32_t, int32_t>> position(
        uint32_t handle) const;
    std::optional<std::pair<uint32_t, uint32_t>> size(
        uint32_t handle) const;
    bool refresh(uint32_t handle);
    [[nodiscard]] std::optional<bool> visible(uint32_t handle) const;
    [[nodiscard]] std::optional<std::string> title(uint32_t handle) const;
    [[nodiscard]] std::optional<std::string> parameter(uint32_t handle) const;
    [[nodiscard]] std::optional<bool> dirty(uint32_t handle) const;

private:
    struct Window {
        bool active = false;
        bool visible = false;
        bool dirty = false;
        std::string title;
        std::string parameter;
        int32_t x = 0;
        int32_t y = 0;
        uint32_t width = 0;
        uint32_t height = 0;
    };

    [[nodiscard]] Window* lookup(uint32_t handle);
    [[nodiscard]] const Window* lookup(uint32_t handle) const;

    std::array<Window, 8> windows_{};
};

}  // namespace bgiplayer
