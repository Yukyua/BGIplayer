#pragma once
#include "bgiplayer/glyph.hpp"
#include <cstdint>
namespace bgiplayer {
// Original0040CED0/0040CF20: already clipped format2 sources, format1 target.
// Validated mix range0..256; opacity>=256 leaves the destination unchanged.
bool blend_original_sprite_crossfade(GlyphBitmapView destination,
    GlyphBitmapView first, GlyphBitmapView second, int32_t mix, uint32_t opacity);
// PE64 1400416A0 -> 14003C5C0: format2 cache, before affine sampling.
bool blend_original_sprite_cache(GlyphBitmapView destination,
    GlyphBitmapView first, GlyphBitmapView second, int32_t mix);
}
