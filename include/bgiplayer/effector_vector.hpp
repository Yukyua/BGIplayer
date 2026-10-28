#pragma once
#include "bgiplayer/glyph.hpp"
namespace bgiplayer {
// PE64 48D10 format6 normals: signed16 x/y plus uint16 coefficient index.
// Each coefficient packs independent signed16 X/Y multipliers.
bool vector_pe64_effector(GlyphBitmapView destination,GlyphBitmapView source,
                          GlyphBitmapView normal_map,std::span<const uint32_t> coefficients,
                          uint32_t bilinear, bool rectangular_bounds = true);
}
