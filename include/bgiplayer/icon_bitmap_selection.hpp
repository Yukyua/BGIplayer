#pragma once
#include <cstdint>
#include <span>

namespace bgiplayer {
// Original CProcInputIconEx virtual+20 at00453790.
uint32_t select_extended_icon_bitmap(std::span<const uint32_t,49> words,
    uint32_t animation_frame, bool selected, bool hovered);
}
