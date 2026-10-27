#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <span>
namespace bgiplayer {
// Original PE64 140087DF0 / PE32 0044B520. Uniform natural cubic through
// at most100 XYZ points. A two-point path uses wrapped-int32 linear deltas.
std::optional<std::array<uint32_t,3>> evaluate_recorded_display_curve(
    std::span<const std::array<uint32_t,3>> points,uint32_t sample,uint32_t extent=65536);
}
