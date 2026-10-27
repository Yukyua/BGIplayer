#include "bgiplayer/text_measure.hpp"
#include <cmath>
#include <algorithm>
namespace bgiplayer {
namespace {
float snap(float x) {
    const float lo=std::floor(x),fraction=x-lo;
    constexpr float tolerance=0.00006103515625F;
    if(fraction>=1.0F-tolerance)return std::ceil(x);
    if(fraction<=tolerance)return lo;
    return x;
}
}
int32_t text_integer_font_spacing(uint32_t proportional,uint32_t bold,
    uint32_t first_init,uint32_t second_init,uint32_t global_spacing) {
    if(proportional!=0)return 0;
    return static_cast<int32_t>((bold!=0?second_init:first_init)+global_spacing);
}
float pe64_font_spacing(int32_t height,uint32_t y_scale,int32_t adjustment) {
    const int32_t scaled=static_cast<int32_t>(static_cast<uint32_t>(height)*y_scale)>>16;
    // Original supersample4 and reciprocal.25 cancel exactly before the
    // Q16 multiplier; double -> float conversion precedes 31B20 snapping.
    return snap(static_cast<float>(double(scaled)*double(adjustment)*(1.0/65536.0)));
}
ProportionalGlyphPlacement place_legacy_proportional(const ProportionalGlyphMeasure& glyph,
    uint32_t reciprocal,int32_t cell) {
    if(glyph.character_bitmap)return {0,static_cast<int32_t>(glyph.ink_width),0};
    if(reciprocal!=0) {
        const int32_t quantum=static_cast<int32_t>(reciprocal*static_cast<uint32_t>(cell))>>16;
        const int32_t a=quantum>>1;
        return {a,static_cast<int32_t>(glyph.ink_width),quantum-a};
    }
    return {static_cast<int32_t>(glyph.abc[0]+0.5F),
        static_cast<int32_t>(glyph.abc[1]+0.5F),static_cast<int32_t>(glyph.abc[2]+0.5F)};
}
std::array<int32_t,3> measure_legacy_proportional_fields(
    std::span<const ProportionalGlyphMeasure> glyphs,uint32_t reciprocal,int32_t cell) {
    uint32_t total=0;int32_t first_a=0,trailing=0;bool first=true;
    for(const auto& glyph:glyphs) {
        const auto part=place_legacy_proportional(glyph,reciprocal,cell);
        total+=static_cast<uint32_t>(part.leading)+static_cast<uint32_t>(part.body)+
            static_cast<uint32_t>(part.trailing);
        if(first){first_a=part.leading;first=false;}
        trailing=part.trailing;
    }
    const auto ink=total-static_cast<uint32_t>(trailing);
    return {static_cast<int32_t>(total),static_cast<int32_t>(ink-static_cast<uint32_t>(first_a)),
        static_cast<int32_t>(ink)};
}
ProportionalGlyphPlacement place_pe64_proportional(const ProportionalGlyphMeasure& glyph,
    float spacing,uint32_t reciprocal,int32_t cell) {
    if(glyph.character_bitmap)return {0,static_cast<int32_t>(glyph.ink_width),static_cast<int32_t>(spacing)};
    if(reciprocal!=0) {
        const int32_t quantum=static_cast<int32_t>(reciprocal*static_cast<uint32_t>(cell))>>16;
        const int32_t a=quantum>>1;
        return {a,static_cast<int32_t>(glyph.ink_width),static_cast<int32_t>(spacing)-a+quantum};
    }
    const int32_t a=static_cast<int32_t>(snap(std::max(glyph.abc[0],0.0F)));
    const float ab=glyph.abc[0]+glyph.abc[1];
    const int32_t b=static_cast<int32_t>(std::ceil(snap(ab-static_cast<float>(a))));
    float tail=ab-static_cast<float>(a+b);tail+=glyph.abc[2];tail+=spacing;tail+=0.5F;
    return {a,b,static_cast<int32_t>(tail)};
}
int32_t pe64_proportional_group_width(bool opening,int32_t cursor,int32_t line_left,
    int32_t current_ab,int32_t current_c,int32_t span_metric2) {
    if(cursor==line_left)return current_ab;
    return opening?static_cast<int32_t>(uint32_t(current_ab)+uint32_t(current_c)+uint32_t(span_metric2)):span_metric2;
}
std::array<int32_t,3> measure_pe64_proportional_fields(std::span<const ProportionalGlyphMeasure> glyphs,
    float spacing,uint32_t reciprocal,int32_t cell) {
    uint32_t total=0;int32_t trailing=0,first_a=0;bool first=true;
    const int32_t quantum=static_cast<int32_t>(reciprocal*static_cast<uint32_t>(cell))>>16;
    for(const auto& glyph:glyphs) {
        if(glyph.character_bitmap) {
            total+=glyph.ink_width;trailing=0;
            if(first){first_a=0;first=false;}
            continue;
        }
        int32_t a,b;
        if(reciprocal==0) {
            a=static_cast<int32_t>(snap(glyph.abc[0]));
            const float ab=glyph.abc[0]+glyph.abc[1];
            b=static_cast<int32_t>(std::ceil(snap(ab-static_cast<float>(a))));
            float tail=ab-static_cast<float>(a+b);
            tail=tail+glyph.abc[2];tail=tail+spacing;tail=tail+0.5F;
            trailing=static_cast<int32_t>(tail);
            total+=static_cast<uint32_t>(a)+static_cast<uint32_t>(b)+static_cast<uint32_t>(trailing);
        } else {
            a=quantum>>1;trailing=static_cast<int32_t>(spacing)-a+quantum;
            total+=glyph.ink_width+static_cast<uint32_t>(static_cast<int32_t>(spacing))+static_cast<uint32_t>(quantum);
        }
        if(first){first_a=a;first=false;}
    }
    const auto ink=total-static_cast<uint32_t>(trailing);
    return {static_cast<int32_t>(total),static_cast<int32_t>(ink-static_cast<uint32_t>(first_a)),static_cast<int32_t>(ink)};
}
int32_t measure_pe64_proportional(std::span<const ProportionalGlyphMeasure> glyphs,
    float spacing,uint32_t reciprocal,int32_t cell) {
    return measure_pe64_proportional_fields(glyphs,spacing,reciprocal,cell)[2];
}
}
