#pragma once
#include "bgiplayer/glyph.hpp"
namespace bgiplayer {
// PE64 14004B280: format4 signed Q4 displacement map(s), strength0..256.
// The optional second map interpolates displacement, not resulting colors.
bool displace_pe64_effector(GlyphBitmapView destination, GlyphBitmapView source,
                           GlyphBitmapView primary, const GlyphBitmapView* secondary,
                           uint32_t strength, uint32_t bilinear);
}
