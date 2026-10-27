#pragma once
#include "bgiplayer/glyph.hpp"
namespace bgiplayer {
// PE64 14004EA70 -> 14004C0D0(extra=0): modes0..7 use14004C310,
// modes>=8 use periodic14004C140 with mode&7, including all numeric aliases.
// Source is placed at x/y; mask_left/top select the mask's destination-space
// origin after BackF's dirty-rectangle crop. Only format1/1 + format3 admitted.
bool compose_original_background_mask(GlyphBitmapView destination,
    GlyphBitmapView source, GlyphBitmapView mask, int32_t x, int32_t y,
    int32_t mask_left, int32_t mask_top, uint32_t mode, uint32_t opacity);
}
