#pragma once
#include "bgiplayer/glyph.hpp"
namespace bgiplayer {
// Original0041D6B0->0041D6D0 style1 shadow colorization. Keeps source alpha,
// replaces RGB. Originalcopies source dimensions; destination must contain it.
bool colorize_glyph_shadow(GlyphBitmapView destination,GlyphBitmapView source,uint32_t color);
// Original00438520->0040A020 style2 glyph outline. The equalradius1..5 branch
// uses immutable copies of original00408600-generated16.16 weighttables.
// Coverage sums EACH (mask*weight)>>16, then saturates255; it is not maxdilation.
// Unequal or larger radii use the original general ellipse kernel.
bool colorize_glyph_outline(GlyphBitmapView destination,const GlyphMask& glyph,
                           uint32_t radius_x,uint32_t radius_y,uint32_t color);
// PE64 140072AE0 gaiji bitmap cache, global1D8C14=0 branch: rectangular alpha sum, saturation255.
bool colorize_pe64_gaiji_outline(GlyphBitmapView destination,const GlyphMask& glyph,
                           uint32_t radius_x,uint32_t radius_y,uint32_t color);
} // namespace bgiplayer
