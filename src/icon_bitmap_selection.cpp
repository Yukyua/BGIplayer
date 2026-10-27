#include "bgiplayer/icon_bitmap_selection.hpp"

namespace bgiplayer {
uint32_t select_extended_icon_bitmap(std::span<const uint32_t,49> words,
    uint32_t animation_frame, bool selected, bool hovered) {
    uint32_t bitmap = words[8];
    const uint32_t flags = words[48];
    bool animate = (flags & 1U) == 0 && ((flags & 8U) == 0 || selected);
    if (hovered && words[9] != 0xFFFFFFFFU) {
        bitmap = words[9];
        animate = true;
    }
    if (selected) {
        const bool selected_bitmap = words[10] != 0xFFFFFFFFU;
        if (selected_bitmap) bitmap = words[10];
        if (hovered) {
            if (words[11] != 0xFFFFFFFFU) bitmap = words[11];
            else if (!animate || ((flags & 1U) != 0 && selected_bitmap)) return bitmap;
        }
    }
    return bitmap + (animate ? animation_frame : 0U);
}
}
