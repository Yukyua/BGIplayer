#include "bgiplayer/text_layout.hpp"
#include "bgiplayer/text_markup.hpp"
#include <algorithm>
#include "bgiplayer/text_measure.hpp"
#include <bit>
#include <limits>
namespace bgiplayer {
PlainTextLayoutResult layout_plain_proportional_line(std::u16string_view text,
    const PlainTextLayoutParameters& p,const GlyphFontRequest& font,
    const GlyphOutlineProvider& provider,uint32_t reciprocal) {
    const auto unsupported=[](){return PlainTextLayoutResult{PlainTextLayoutError::UnsupportedControlOrTag,std::nullopt};};
    std::optional<std::u16string> fallback;
    if(has_complete_text_markup(text)){fallback=fallback_unsupported_text_markup(text);if(!fallback)return unsupported();text=*fallback;}
    if(!text.empty() && ((text.front()>=2 && text.front()<=8) || text.front()==0x0B))return unsupported();
    struct Item{GlyphMask glyph;ProportionalGlyphMeasure metrics;GlyphBitmapView trim;uint32_t codepoint;size_t source_index;};
    std::vector<Item> items;std::vector<ProportionalGlyphMeasure> metrics;
    for(size_t source_index=0;source_index<text.size();++source_index) {
        const char16_t c=text[source_index];
        if(c==0)break;
        if(c<0xE && c!='\n')continue;
        if(c=='\n' || (c>=0xd800 && c<=0xdfff))return unsupported();
        auto glyph=load_glyph_outline(provider,font,c);
        if(!glyph)return {PlainTextLayoutError::MissingGlyph,std::nullopt};
        GlyphBitmapView view{{},0,glyph->width*4U,glyph->width,glyph->height,2,4};
        trim_glyph_bitmap_view(view,glyph->metrics);
        ProportionalGlyphMeasure metric{{std::bit_cast<float>(glyph->metrics.words[7]),
            std::bit_cast<float>(glyph->metrics.words[8]),std::bit_cast<float>(glyph->metrics.words[9])},glyph->character_bitmap?glyph->character_bitmap_source_width:view.width,glyph->character_bitmap};
        metrics.push_back(metric);items.push_back({std::move(*glyph),metric,view,c,source_index});
    }
    const int64_t limit=int64_t(p.bounds_right)+1;
    // No grouping can wrap if the complete span and every glyph fit this line.
    if(int64_t(p.cursor_x)+measure_pe64_proportional(metrics,p.font_spacing,reciprocal,p.font_cell_width)>limit)
        return unsupported();
    PlainTextLayout result{{},p.cursor_x,p.cursor_y,1};uint32_t delay=0;
    size_t word_remaining=0;
    for(size_t index=0;index<items.size();++index) {
        const auto& item=items[index];
        if(item.glyph.character_bitmap && item.glyph.character_bitmap_source_width==0)continue;
        if(p.ascii_word_grouping && word_remaining==0) {
            const auto run=target_ascii_word_run(text.substr(item.source_index));
            if(run!=0) {
                if(int64_t(result.cursor_x)+measure_pe64_proportional(
                    std::span<const ProportionalGlyphMeasure>(metrics).subspan(index,run),
                    p.font_spacing,reciprocal,p.font_cell_width)>limit)return unsupported();
                word_remaining=run;
            }
        }
        if(word_remaining!=0)--word_remaining;
        auto parts=place_pe64_proportional(item.metrics,p.font_spacing,reciprocal,p.font_cell_width);
        if(item.glyph.character_bitmap)parts.body=item.glyph.character_bitmap_scaled?font.height:static_cast<int32_t>(item.glyph.character_bitmap_source_width);
        if(int64_t(result.cursor_x)+parts.leading+parts.body>limit)return unsupported();
        const auto& glyph=item.glyph;
        std::vector<uint8_t> full(size_t(glyph.width)*glyph.height*4U);
        GlyphBitmapView fullview{full,0,glyph.width*4U,glyph.width,glyph.height,2,4};
        if(!colorize_glyph_mask(fullview,glyph,p.color))return {PlainTextLayoutError::InvalidState,std::nullopt};
        const uint32_t w=item.trim.width;
        if(w>glyph.width || item.trim.byte_offset>full.size())return {PlainTextLayoutError::InvalidState,std::nullopt};
        PlainTextGlyphNode node{item.codepoint,result.cursor_x+parts.leading,
            result.cursor_y+p.baseline_offset,delay,0,p.fade_duration,0,w,glyph.height,
            std::vector<uint8_t>(size_t(w)*glyph.height*4U)};
        for(uint32_t y=0;y<glyph.height;++y){const size_t source=item.trim.byte_offset+size_t(y)*glyph.width*4U;
            if(source+size_t(w)*4U>full.size())return {PlainTextLayoutError::InvalidState,std::nullopt};
            std::copy_n(full.data()+source,size_t(w)*4U,node.bitmap.data()+size_t(y)*w*4U);}
        result.nodes.push_back(std::move(node));
        result.cursor_x=static_cast<int32_t>(uint32_t(result.cursor_x)+uint32_t(parts.leading)+uint32_t(parts.body)+uint32_t(parts.trailing));
        delay+=p.character_delay;
    }
    return {PlainTextLayoutError::None,std::move(result)};
}
}
