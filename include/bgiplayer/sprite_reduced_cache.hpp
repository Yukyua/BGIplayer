#pragma once
#include "bgiplayer/glyph.hpp"
namespace bgiplayer {
// PE64 1400550B0 -> 140053E20: ceil-size 2x reduction. PAVGB first
// averages vertically, then horizontally; odd edges use one direction only.
bool reduce_original_sprite_cache(GlyphBitmapView destination,
                                  GlyphBitmapView source);
}
