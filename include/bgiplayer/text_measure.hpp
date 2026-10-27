#pragma once
#include <array>
#include <cstdint>
#include <span>
namespace bgiplayer {
struct ProportionalGlyphMeasure {std::array<float,3> abc;uint32_t ink_width;bool character_bitmap=false;};
struct ProportionalGlyphPlacement {int32_t leading;int32_t body;int32_t trailing;};
// PE64 14006A190: derive and snap the cache's float gap from logical height,
// effective Q16 Y scale and signed Q16 letter-spacing adjustment.
float pe64_font_spacing(int32_t height,uint32_t y_scale,int32_t adjustment);
// PE32 43D800 / PE64 75440: mode!=0 suppresses integer spacing;
// regular/bold select separate cache initializers before global fixed gap.
int32_t text_integer_font_spacing(uint32_t proportional,uint32_t bold,
    uint32_t first_init,uint32_t second_init,uint32_t global_spacing);
// PE32 0043AD50 rounds each ABC component independently; no PE64 bearing clamp.
ProportionalGlyphPlacement place_legacy_proportional(const ProportionalGlyphMeasure& glyph,
    uint32_t reciprocal,int32_t cell);
std::array<int32_t,3> measure_legacy_proportional_fields(
    std::span<const ProportionalGlyphMeasure> glyphs,uint32_t reciprocal,int32_t cell);
// PE64 14007731E clamps the rendered A bearing at0, unlike measurement.
ProportionalGlyphPlacement place_pe64_proportional(const ProportionalGlyphMeasure& glyph,
    float spacing,uint32_t reciprocal,int32_t cell);
// PE64 140077543/14007755B group lookahead preserves current A+B at line start.
int32_t pe64_proportional_group_width(bool opening,int32_t cursor,int32_t line_left,
    int32_t current_ab,int32_t current_c,int32_t span_metric2);
// PE64 1400796C0 outputs total advance, total-finalC-firstA, total-finalC.
std::array<int32_t,3> measure_pe64_proportional_fields(std::span<const ProportionalGlyphMeasure> glyphs,
    float spacing,uint32_t reciprocal,int32_t cell);
// PE64 1400796C0 output+8; Q16 reciprocal zero selects unquantized ABC.
int32_t measure_pe64_proportional(std::span<const ProportionalGlyphMeasure> glyphs,
    float spacing,uint32_t reciprocal,int32_t cell);
}
