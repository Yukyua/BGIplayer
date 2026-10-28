#pragma once
#include "bgiplayer/glyph.hpp"
#include <array>
#include <optional>
namespace bgiplayer {
std::optional<std::array<int32_t,6>> effector_transform_matrix(
    uint32_t width,uint32_t height,const std::array<int32_t,5>& transform);
// PE64 5BD50 kind3 ->52B50: source originX/Y, angle, unsigned Q16 X/Y scale.
bool transform_pe64_effector(GlyphBitmapView destination,GlyphBitmapView source,
                             const std::array<int32_t,5>& transform,uint32_t bilinear);
}
