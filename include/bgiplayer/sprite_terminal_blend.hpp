#pragma once
#include "bgiplayer/glyph.hpp"
namespace bgiplayer {
// PE64 14003E490 -> 14003BD50 / 14003B3A0, format2-to-format1.
// Modes2/3 are saturated RGB add/subtract;21/22 complement the parameter.
// Alpha is quantized to7 bits; the target's fourth byte is preserved.
bool blend_original_sprite_terminal(GlyphBitmapView destination,
                                    GlyphBitmapView source,
                                    uint32_t mode, uint32_t parameter);
}
