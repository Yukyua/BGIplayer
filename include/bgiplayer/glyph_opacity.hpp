#pragma once
#include "bgiplayer/glyph.hpp"
namespace bgiplayer {
// Original0040CB30 format2->2 nonzero opacity kernel: paired SSE reciprocal
// weights and separate scalar odd tail, original alpha composed independently.
bool blend_original_glyph_opacity(GlyphBitmapView destination,GlyphBitmapView source,
                                 int32_t x,int32_t y,uint32_t parameter);
}
