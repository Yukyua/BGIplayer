#pragma once
#include "bgiplayer/glyph.hpp"
namespace bgiplayer {
// PE64 4B1A0 mode5: horizontal49510 then vertical48E30 (flag1).
bool blur_pe64_effector(GlyphBitmapView destination,GlyphBitmapView source,uint32_t radius);
}
