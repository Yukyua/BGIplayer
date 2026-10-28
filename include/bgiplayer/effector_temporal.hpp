#pragma once
#include "bgiplayer/glyph.hpp"
namespace bgiplayer {
// PE64 5BD50 kind4: blend scene with retained history, then retain output.
bool temporal_pe64_effector(GlyphBitmapView scene,GlyphBitmapView history,uint32_t strength);
}
