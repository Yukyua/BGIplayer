#include "bgiplayer/text_layout.hpp"
#include "bgiplayer/text_markup.hpp"
#include "bgiplayer/text_measure.hpp"
#include <bit>
#include <algorithm>
#include <limits>

namespace bgiplayer {
namespace {
int32_t wrap_add(int32_t a,int32_t b){return static_cast<int32_t>(static_cast<uint32_t>(a)+static_cast<uint32_t>(b));}
int32_t wrap_sub(int32_t a,int32_t b){return static_cast<int32_t>(static_cast<uint32_t>(a)-static_cast<uint32_t>(b));}
bool quote_before_nonspace(std::u16string_view text,std::size_t index){
    return index<text.size() && (text[index]=='"' || text[index]=='\'') && index+1<text.size() &&
           text[index+1]!=0 && text[index+1]!=' ' && text[index+1]!=0x3000;
}
}
PlainTextLayoutResult layout_legacy_text(std::u16string_view text,
    const FixedKinsokuLayoutParameters& parameters,const GlyphFontRequest& font,
    const GlyphOutlineProvider& glyphs,bool proportional,uint32_t reciprocal,
    uint32_t right_extension,std::vector<GlyphMask>* style_sources,bool ruby_enabled,
    std::span<const TextRubyAlias> aliases) {
    const auto& p=parameters.plain;
    if(p.font_cell_width<=0 || parameters.font_height<=0 || p.bounds_right<p.bounds_left ||
       p.bounds_right==std::numeric_limits<int32_t>::max() || parameters.reserved_right_margin<0)
        return {PlainTextLayoutError::InvalidState,std::nullopt};
    bool suppress_hanging=false,explicit_indent=false,ascii_grouping=p.ascii_word_grouping;
    while(!text.empty()) {
        const auto code=text.front();
        if(code==3)suppress_hanging=true;
        else if(code==4)explicit_indent=true;
        else if(code==0x0B)ascii_grouping=false;
        else if(code==2 || (code>=5 && code<=8))
            return {PlainTextLayoutError::UnsupportedControlOrTag,std::nullopt};
        else break;
        text.remove_prefix(1);
    }
    const auto initial_character=text.empty()?char16_t(0):text.front();
    auto parsed=parse_text_ruby(text,ruby_enabled?aliases:std::span<const TextRubyAlias>{});
    if(!parsed) {
        auto body=fallback_unsupported_text_markup(text);
        if(!body)return {PlainTextLayoutError::UnsupportedControlOrTag,std::nullopt};
        parsed=ParsedRubyText{std::move(*body),{}, {}};
    }
    text=parsed->visible;
    struct Item {GlyphMask glyph;ProportionalGlyphMeasure metric;ProportionalGlyphPlacement placement;};
    std::vector<std::optional<Item>> items;std::vector<ProportionalGlyphMeasure> metrics;
    for(char16_t c:text) {
        if(c==0)break;
        if(c>=0xD800 && c<=0xDFFF)return {PlainTextLayoutError::UnsupportedControlOrTag,std::nullopt};
        if(c<0xE){items.emplace_back();metrics.push_back({{},0});continue;}
        auto glyph=load_glyph_outline(glyphs,font,c);
        if(!glyph)return {PlainTextLayoutError::MissingGlyph,std::nullopt};
        if(proportional && glyph->metrics.ink_left()>glyph->width)
            return {PlainTextLayoutError::InvalidState,std::nullopt};
        GlyphBitmapView view{{},0,glyph->width*4U,glyph->width,glyph->height,2,4};
        if(proportional)trim_glyph_bitmap_view(view,glyph->metrics);
        ProportionalGlyphMeasure metric{{std::bit_cast<float>(glyph->metrics.words[7]),
            std::bit_cast<float>(glyph->metrics.words[8]),std::bit_cast<float>(glyph->metrics.words[9])},glyph->character_bitmap?glyph->character_bitmap_source_width:view.width,glyph->character_bitmap};
        const auto part=proportional?place_legacy_proportional(metric,reciprocal,p.font_cell_width):
            ProportionalGlyphPlacement{0,glyph->character_bitmap?static_cast<int32_t>(glyph->width):
                target_wide_character(c)?p.font_cell_width:p.font_cell_width/2,
                glyph->character_bitmap?static_cast<int32_t>(p.font_spacing):0};
        metrics.push_back(metric);items.push_back(Item{std::move(*glyph),metric,part});
    }
    const auto measure=[&](std::size_t start,std::size_t count){
        if(proportional)return measure_legacy_proportional_fields(
            std::span<const ProportionalGlyphMeasure>(metrics).subspan(start,count),reciprocal,p.font_cell_width);
        int32_t width=wrap_sub(0,p.character_spacing);
        for(std::size_t i=start;i<start+count;++i)if(items[i])
            width=wrap_add(width,wrap_add(items[i]->placement.body,p.character_spacing));
        return std::array<int32_t,3>{width,width,width};
    };
    const auto boundary=[&](std::size_t at){return std::find(parsed->boundaries.begin(),parsed->boundaries.end(),at)!=parsed->boundaries.end();};
    const auto group_run=[&](std::size_t start){
        auto end=text.size();
        for(auto at:parsed->boundaries)if(at>start){end=at;break;}
        return target_ascii_word_run(text.substr(start,end-start));
    };
    PlainTextLayout result{{},p.cursor_x,p.cursor_y,1};
    int32_t hanging=0;
    // 0043B0E5..B173 applies explicit04's U+300C reference before Ruby
    // line indentation. 03 suppresses both explicit and automatic hanging.
    if(parameters.kinsoku && parameters.initial_hanging_indent && !suppress_hanging &&
       (explicit_indent || target_initial_hanging_character(initial_character))) {
        const auto reference=explicit_indent?char16_t(0x300C):initial_character;
        if(proportional) {
            auto glyph=load_glyph_outline(glyphs,font,reference);
            if(!glyph)return {PlainTextLayoutError::MissingGlyph,std::nullopt};
            if(glyph->metrics.ink_left()>glyph->width)return {PlainTextLayoutError::InvalidState,std::nullopt};
            GlyphBitmapView view{{},0,glyph->width*4U,glyph->width,glyph->height,2,4};
            trim_glyph_bitmap_view(view,glyph->metrics);
            const ProportionalGlyphMeasure metric{{std::bit_cast<float>(glyph->metrics.words[7]),
                std::bit_cast<float>(glyph->metrics.words[8]),std::bit_cast<float>(glyph->metrics.words[9])},glyph->character_bitmap?glyph->character_bitmap_source_width:view.width,glyph->character_bitmap};
            hanging=measure_legacy_proportional_fields(std::span(&metric,1),reciprocal,p.font_cell_width)[0];
        } else hanging=wrap_add(target_wide_character(reference)?p.font_cell_width:p.font_cell_width/2,p.character_spacing);
        if(explicit_indent && result.cursor_x==p.bounds_left)result.cursor_x=wrap_add(result.cursor_x,hanging);
    }
    if(result.cursor_x==p.bounds_left)result.cursor_x=wrap_add(result.cursor_x,parameters.line_indent);
    const int32_t continuation_indent=wrap_add(parameters.line_indent,hanging);
    std::size_t word_remaining=0,kinsoku_remaining=0;
    bool after_break=false;
    uint32_t delay=0;
    const auto newline=[&](bool reset_groups){result.cursor_x=wrap_add(p.bounds_left,continuation_indent);
        result.cursor_y=wrap_add(result.cursor_y,p.line_advance);++result.lines;after_break=true;
        if(reset_groups){word_remaining=0;kinsoku_remaining=0;}};
    const int32_t margin=parameters.kinsoku?parameters.reserved_right_margin:0;
    const int32_t primary_limit=wrap_add(wrap_sub(p.bounds_right,margin),1);
    for(std::size_t index=0;index<items.size();++index){
        const char16_t c=text[index];
        if(c==0)break;
        if(c>=0xD800 && c<=0xDFFF)
            return {PlainTextLayoutError::UnsupportedControlOrTag,std::nullopt};
        if(c<0xE && c!='\n'){after_break=false;continue;}
        if(c=='\n'){newline(false);continue;}
        if(items[index]->glyph.character_bitmap && items[index]->glyph.width==0)continue;
        const auto& item=*items[index];const auto part=item.placement;
        const int32_t cell=wrap_add(part.leading,part.body);
        const int32_t advance=wrap_add(wrap_add(cell,part.trailing),proportional?0:p.character_spacing);
        const char16_t space=target_wide_character(c)?u'\u3000':u' ';
        if(parameters.kinsoku && c==space && index!=0 && text[index-1]!=space &&
           wrap_add(wrap_sub(p.bounds_right,parameters.font_height),1)<wrap_add(result.cursor_x,
               target_wide_character(c)?p.font_cell_width:p.font_cell_width/2)){
            newline(false);continue; // 0043C6xx consumes this space, no node/delay.
        }
        int32_t wrap_width=cell,trailing_width=0;
        bool opening=parameters.kinsoku && kinsoku_remaining==0 && target_kinsoku_opening(c);
        bool opening_pair=opening,group_boundary=true;
        std::size_t next=index+1;
        if(ascii_grouping){
            if(word_remaining==0){
                const auto start=opening?index+1:index;
                const auto count=group_run(start);
                if(count!=0){
                    const int32_t width=measure(start,count)[2];
                    if(opening){wrap_width=wrap_add(wrap_add(wrap_width,part.trailing),width);word_remaining=count;}
                    else{wrap_width=width;word_remaining=count-1;}
                    next=start+count;opening_pair=false;
                }
            }else{--word_remaining;group_boundary=false;}
        }
        const auto ruby=std::find_if(parsed->ruby.begin(),parsed->ruby.end(),
            [&](const auto& run){return run.first==index;});
        if(ruby_enabled && ruby!=parsed->ruby.end()) {
            if(ruby->reading.find_first_of(u"<\n")!=ruby->reading.npos ||
               text.substr(index,ruby->count).find('\n')!=text.npos)
                return {PlainTextLayoutError::UnsupportedControlOrTag,std::nullopt};
            wrap_width=std::max(wrap_width,measure(index,ruby->count)[2]);
            next=index+ruby->count;word_remaining=ruby->count-1;opening_pair=false;
        }
        if(parameters.kinsoku){
            if(kinsoku_remaining==0){
                if(group_boundary){
                    auto count=target_kinsoku_run(text.substr(next));
                    for(std::size_t i=0;i<count;++i)if(boundary(next+i)){count=i;break;}
                    if(count!=0 && !(count==1 && quote_before_nonspace(text,next))){
                        const auto span=text.substr(next,count);
                        trailing_width=measure(next,count)[2];
                        std::size_t prefix=0;
                        for(std::size_t i=0;i<span.size();++i)
                            if(!target_kinsoku_line_start(span[i]))prefix=i+1;
                        if(prefix!=0){const auto width=measure(next,prefix)[2];
                            wrap_width=wrap_add(wrap_width,width);trailing_width=wrap_sub(trailing_width,width);}
                        kinsoku_remaining=word_remaining+count;word_remaining=kinsoku_remaining;
                        opening_pair=false;
                    }
                }
            }else --kinsoku_remaining;
        }
        if(opening_pair) {
            // 0043A650 on an empty trailing string returns -spacing: it
            // subtracts final spacing even when the loop measured no glyphs.
            if(next<text.size() && text[next]!=0 && !boundary(next)) {
                if(items[next])wrap_width=wrap_add(wrap_width,measure(next,1)[0]);
                else if(proportional) {
                    // 43CFxx measures the next code unit even when it is a
                    // body control such as LF; only the draw loop skips it.
                    auto glyph=load_glyph_outline(glyphs,font,text[next]);
                    if(!glyph)return {PlainTextLayoutError::MissingGlyph,std::nullopt};
                    if(glyph->metrics.ink_left()>glyph->width)return {PlainTextLayoutError::InvalidState,std::nullopt};
                    GlyphBitmapView view{{},0,glyph->width*4U,glyph->width,glyph->height,2,4};
                    trim_glyph_bitmap_view(view,glyph->metrics);
                    const ProportionalGlyphMeasure metric{{std::bit_cast<float>(glyph->metrics.words[7]),
                        std::bit_cast<float>(glyph->metrics.words[8]),std::bit_cast<float>(glyph->metrics.words[9])},glyph->character_bitmap?glyph->character_bitmap_source_width:view.width,glyph->character_bitmap};
                    wrap_width=wrap_add(wrap_width,measure_legacy_proportional_fields(
                        std::span(&metric,1),reciprocal,p.font_cell_width)[0]);
                } else wrap_width=wrap_add(wrap_width,
                    target_wide_character(text[next])?p.font_cell_width:p.font_cell_width/2);
            } else wrap_width=wrap_add(wrap_width,measure(next,0)[0]);
        }
        wrap_width=wrap_add(wrap_width,parameters.style_wrap_extension);
        bool do_wrap=primary_limit<wrap_add(result.cursor_x,wrap_width);
        bool retest=!do_wrap && trailing_width>0;
        if(do_wrap && target_kinsoku_line_start(c) && !quote_before_nonspace(text,index))retest=true;
        if(retest){
            const int32_t hanging_margin=parameters.allow_reserved_margin_for_punctuation?0:margin;
            const int32_t limit=wrap_add(wrap_sub(p.bounds_right,hanging_margin),1);
            do_wrap=limit<wrap_add(wrap_add(result.cursor_x,wrap_width),trailing_width);
        }
        if(do_wrap)newline(true);
        if(p.trim_line_leading_space && after_break && c==space){after_break=false;continue;}
        if(ruby_enabled && ruby!=parsed->ruby.end())
            result.ruby_runs.push_back({result.nodes.size(),ruby->reading,
                wrap_add(result.cursor_x,part.leading),wrap_add(result.cursor_y,p.baseline_offset),
                measure(index,ruby->count)[1],static_cast<uint32_t>(ruby->count),delay});
        const auto& glyph=item.glyph;
        const uint32_t left=proportional?glyph.metrics.ink_left():0U;
        const uint32_t width=proportional?std::min(glyph.width-left,item.metric.ink_width+right_extension):glyph.width;
        const uint64_t bytes=static_cast<uint64_t>(glyph.width)*glyph.height*4U;
        if(glyph.width>std::numeric_limits<uint32_t>::max()/4U || bytes>std::numeric_limits<std::size_t>::max())
            return {PlainTextLayoutError::InvalidState,std::nullopt};
        std::vector<uint8_t> full(static_cast<std::size_t>(bytes));
        GlyphBitmapView view{full,0,glyph.width*4U,glyph.width,glyph.height,2,4};
        if(!colorize_glyph_mask(view,glyph,p.color))return {PlainTextLayoutError::InvalidState,std::nullopt};
        PlainTextGlyphNode node{c,wrap_add(result.cursor_x,part.leading),wrap_add(result.cursor_y,p.baseline_offset),delay,0,
            p.fade_duration,0,width,glyph.height,std::vector<uint8_t>(std::size_t(width)*glyph.height*4U)};
        for(uint32_t y=0;y<glyph.height;++y)std::copy_n(full.data()+(std::size_t(y)*glyph.width+left)*4U,
            std::size_t(width)*4U,node.bitmap.data()+std::size_t(y)*width*4U);
        result.nodes.push_back(std::move(node));
        if(style_sources)style_sources->push_back(!proportional && !glyph.character_bitmap?GlyphMask{}:glyph);
        result.cursor_x=wrap_add(result.cursor_x,advance);
        delay+=p.character_delay;after_break=false;
    }
    return {PlainTextLayoutError::None,std::move(result)};
}
PlainTextLayoutResult layout_plain_fixed_kinsoku_text(std::u16string_view text,
    const FixedKinsokuLayoutParameters& parameters,const GlyphFontRequest& font,
    const GlyphOutlineProvider& glyphs) {
    return layout_legacy_text(text,parameters,font,glyphs,false,0);
}
} // namespace bgiplayer
