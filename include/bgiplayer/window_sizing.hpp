#pragma once
#include <cstdint>
#include <optional>
#include <utility>
namespace bgiplayer {
// PE64 140101030: independent sizing limit; does not change canvas resolution.
std::optional<std::pair<uint32_t,uint32_t>> checked_window_sizing_limit(
    uint32_t canvas_width,uint32_t canvas_height,int32_t width,int32_t height);
}
