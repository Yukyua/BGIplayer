#pragma once
#include "bgiplayer/glyph.hpp"
namespace bgiplayer {
// PE64 14004B1A0 selectors 0..5: horizontal, vertical, or both axes;
// even selectors use zero beyond the source, odd selectors clamp edges.
bool blur_pe64_effector_selector(GlyphBitmapView destination,
                                    GlyphBitmapView source,
                                    uint32_t radius, uint32_t selector);
}
