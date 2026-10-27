#include "bgiplayer/text_layout.hpp"
#include "bgiplayer/text_measure.hpp"
#include "bgiplayer/text_markup.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
namespace bgiplayer {
namespace {
int32_t plus(int32_t a,int32_t b){return static_cast<int32_t>(uint32_t(a)+uint32_t(b));}
int32_t minus(int32_t a,int32_t b){return static_cast<int32_t>(uint32_t(a)-uint32_t(b));}
bool quote_nonspace(std::u16string_view text,size_t i){return i<text.size() && (text[i]=='"'||text[i]=='\'') && i+1<text.size() && text[i+1] && text[i+1]!=' ' && text[i+1]!=0x3000;}
// UTF16 tables in the PE64 image. These differ from the earlier PE32 tables.
constexpr std::u16string_view opening=u"[{(（〔［｛〈≪《「『【“";
constexpr std::u16string_view initial=u"「　（『‘“(\"";
constexpr std::u16string_view line_start=u"\"',.?!ﾞﾟ，．、。？！”゛゜]})）〕］｝〉≫》」』】ヽヾゝゞ々・‥…━―─ー～♪ぁぃぅぇぉっゃゅょァィゥェォッャュョ　";
constexpr std::u16string_view prohibited_run=u"\u0022\u0027\u002c\u002e\u003a\u003b\u003f\u0021\uff9e\uff9f\uff65\uff0c\uff0e\u3001\u3002\uff1a\uff1b\uff1f\uff01\u201d\u309b\u309c\u2010\u005d\u007d\u0029\uff09\u3015\uff3d\uff5d\u3009\u226b\u300b\u300d\u300f\u3011\u30fd\u30fe\u309d\u309e\u3005\u30fb\u2025\u2026\u2501\u2015\u2500\u30fc\uff5e\u266a\u3041\u3043\u3045\u3047\u3049\u3063\u3083\u3085\u3087\u30a1\u30a3\u30a5\u30a7\u30a9\u30c3\u30e3\u30e5\u30e7\u3092\u30f2\u3000";
bool member(std::u16string_view chars,char16_t c){return chars.find(c)!=chars.npos;}
size_t ascii_run(std::u16string_view text){size_t n=0;while(n<text.size()) {const uint16_t c=text[n];if(c!='<' && ((c>0x20 && c<0x7f)||(c>=0xae && c<=0xfff)))++n;else break;}if(n<text.size() && text[n]==0xad)++n;return n;}

}
static PlainTextLayoutResult layout_pe64_text(std::u16string_view text,
    const FixedKinsokuLayoutParameters& parameters,const GlyphFontRequest& font,
    const GlyphOutlineProvider& provider,uint32_t reciprocal,uint32_t right_extension,
    std::vector<GlyphMask>* style_sources,bool proportional,bool ruby_enabled,
    std::span<const TextRubyAlias> aliases) {
    const auto& p=parameters.plain;
    const auto failure=[](PlainTextLayoutError e){return PlainTextLayoutResult{e,std::nullopt};};
    // A private slot uses its own width (or logical height in scaled mode).
    // Original 77036 accepts it even when the normal font cell truncates to0.
    const bool private_only=std::any_of(text.begin(),text.end(),[](char16_t c){return c>=0xF001U && c<=0xF7FFU;}) &&
        std::all_of(text.begin(),text.end(),[](char16_t c){return c<0x0e || (c>=0xF001U && c<=0xF7FFU);});
    if(p.font_cell_width<0 || (p.font_cell_width==0 && !private_only) || p.bounds_right<p.bounds_left || p.bounds_right==INT32_MAX)
        return failure(PlainTextLayoutError::InvalidState);
    // 140075686..757F2: leading controls consume statefully until an
    // unrecognized prefix terminates this loop. Body controls have a separate
    // dispatch and must not inherit these leading meanings.
    bool suppress_hanging=false,ascii_grouping=p.ascii_word_grouping;
    bool explicit_indent=false;
    while(!text.empty()) {
        const char16_t code=text.front();
        if(code==3)suppress_hanging=true;
        else if(code==0x0b)ascii_grouping=false;
        else if(code==4)explicit_indent=true; // reference U+300C, 140075707.
        else if(code==2 || (code>=5 && code<=8) || code==0x0e)
            return failure(PlainTextLayoutError::UnsupportedControlOrTag);
        else break;
        text.remove_prefix(1);
    }
    const char16_t initial_character=text.empty()?0:text.front();
    auto parsed=parse_text_ruby(text,ruby_enabled?aliases:std::span<const TextRubyAlias>{});
    if(parsed && ruby_enabled)for(const auto& run:parsed->ruby) {
        const auto base=std::u16string_view(parsed->visible).substr(run.first,run.count);
        if(run.reading.find_first_of(u"<\n")!=run.reading.npos || base.find('\n')!=base.npos) {
            parsed.reset();break;
        }
    }
    if(!parsed) {
        auto body=fallback_unsupported_text_markup(text);
        if(!body)return failure(PlainTextLayoutError::UnsupportedControlOrTag);
        parsed=ParsedRubyText{std::move(*body),{}, {}};
    }
    text=parsed->visible;
    const auto group_run=[&](size_t start){
        auto end=text.size();
        for(size_t boundary:parsed->boundaries)if(boundary>start){end=boundary;break;}
        return ascii_run(text.substr(start,end-start));
    };
    const auto tag_boundary=[&](size_t index){return std::find(parsed->boundaries.begin(),parsed->boundaries.end(),index)!=parsed->boundaries.end();};
    struct Item {GlyphMask glyph;ProportionalGlyphMeasure metric;ProportionalGlyphPlacement placement;};
    std::vector<std::optional<Item>> items;std::vector<ProportionalGlyphMeasure> metrics;
    for(char16_t c:text) {
        if(!c)break;
        if((c>=0xd800&&c<=0xdfff))return failure(PlainTextLayoutError::UnsupportedControlOrTag);
        if(c<0x0e){items.emplace_back();metrics.push_back({{},0});continue;}
        auto glyph=load_glyph_outline(provider,font,c);if(!glyph)return failure(PlainTextLayoutError::MissingGlyph);
        if(proportional && glyph->metrics.ink_left()>glyph->width)return failure(PlainTextLayoutError::InvalidState);
        GlyphBitmapView view{{},0,glyph->width*4U,glyph->width,glyph->height,2,4};
        if(proportional)trim_glyph_bitmap_view(view,glyph->metrics);
        ProportionalGlyphMeasure metric{{std::bit_cast<float>(glyph->metrics.words[7]),std::bit_cast<float>(glyph->metrics.words[8]),std::bit_cast<float>(glyph->metrics.words[9])},glyph->character_bitmap?glyph->character_bitmap_source_width:view.width,glyph->character_bitmap};
        auto placement=proportional ? place_pe64_proportional(metric,p.font_spacing,reciprocal,p.font_cell_width) :
            ProportionalGlyphPlacement{0,glyph->character_bitmap?static_cast<int32_t>(glyph->width):
                target_wide_character(c)?p.font_cell_width:p.font_cell_width/2,
                glyph->character_bitmap?static_cast<int32_t>(p.font_spacing):0};
        if(glyph->character_bitmap)placement.body=glyph->character_bitmap_scaled?font.height:static_cast<int32_t>(glyph->character_bitmap_source_width);
        items.push_back(Item{std::move(*glyph),metric,placement});metrics.push_back(metric);
    }
    const auto measure=[&](size_t start,size_t count){
        if(proportional)return measure_pe64_proportional_fields(std::span<const ProportionalGlyphMeasure>(metrics).subspan(start,count),p.font_spacing,reciprocal,p.font_cell_width);
        // 1400798DD..79918 fixed cells; 79972..7999B subtracts the
        // final spacing. No ABC bearings: all three metrics coincide.
        int32_t width=0;
        for(size_t i=start;i<start+count;++i)
            if(items[i])width=plus(width,plus(items[i]->glyph.character_bitmap?
                static_cast<int32_t>(items[i]->glyph.character_bitmap_source_width):items[i]->placement.body,p.character_spacing));
        width=minus(width,p.character_spacing);
        return std::array<int32_t,3>{width,width,width};
    };
    PlainTextLayout result{{},p.cursor_x,p.cursor_y,1};
    int32_t hanging=0;
    // Original 140075948..75ADD measures the reference, adds integer
    // font gap (75440 returns zero for proportional), and moves the first
    // cursor only for an explicit override while it is still at line left.
    if(parameters.kinsoku && parameters.initial_hanging_indent && !suppress_hanging &&
       (explicit_indent || member(initial,initial_character))) {
        const char16_t reference=explicit_indent?u'「':initial_character;
        auto glyph=load_glyph_outline(provider,font,reference);
        if(!glyph)return failure(PlainTextLayoutError::MissingGlyph);
        if(proportional) {
            if(glyph->metrics.ink_left()>glyph->width)return failure(PlainTextLayoutError::InvalidState);
            GlyphBitmapView view{{},0,glyph->width*4U,glyph->width,glyph->height,2,4};
            trim_glyph_bitmap_view(view,glyph->metrics);
            const ProportionalGlyphMeasure metric{
                {std::bit_cast<float>(glyph->metrics.words[7]),std::bit_cast<float>(glyph->metrics.words[8]),
                 std::bit_cast<float>(glyph->metrics.words[9])},glyph->character_bitmap?glyph->character_bitmap_source_width:view.width,glyph->character_bitmap};
            hanging=measure_pe64_proportional_fields(std::span(&metric,1),p.font_spacing,reciprocal,p.font_cell_width)[0];
        } else hanging=plus(target_wide_character(reference)?p.font_cell_width:p.font_cell_width/2,
                           p.character_spacing);
        if(explicit_indent && result.cursor_x==p.bounds_left)result.cursor_x=plus(result.cursor_x,hanging);
    }
    // The ruby line-indent getter is applied afterwards. If explicit04 has
    // already moved the first cursor, ruby indent only affects later lines.
    if(result.cursor_x==p.bounds_left)result.cursor_x=plus(result.cursor_x,parameters.line_indent);
    const int32_t indent=plus(hanging,parameters.line_indent);
    const int32_t margin=parameters.kinsoku?parameters.reserved_right_margin:0;
    const int32_t primary_limit=plus(minus(p.bounds_right,margin),1);
    size_t word_remaining=0,kinsoku_remaining=0;uint32_t delay=0;bool after_break=false;
    const auto newline=[&]{result.cursor_x=plus(p.bounds_left,indent);result.cursor_y=plus(result.cursor_y,p.line_advance);++result.lines;after_break=true;};
    for(size_t index=0;index<items.size();++index) {
        const char16_t c=text[index];
        if(c=='\n'){newline();continue;}
        if(c<0x0e){after_break=false;continue;} // 140075C69: no glyph/delay.
        if(items[index]->glyph.character_bitmap && items[index]->glyph.character_bitmap_source_width==0)continue;
        const auto& item=*items[index];const auto parts=item.placement;
        const int32_t advance=plus(plus(plus(parts.leading,parts.body),parts.trailing),proportional?0:p.character_spacing);
        const char16_t space=target_wide_character(c)?0x3000:' ';
        if(parameters.kinsoku && c==space && index && text[index-1]!=space &&
           plus(result.cursor_x,parameters.font_height)>plus(minus(p.bounds_right,parameters.font_height),1)){
            newline();continue;
        }
        int32_t primary=plus(parts.leading,parts.body),combined=primary,trailing=0;
        const auto ruby=std::find_if(parsed->ruby.begin(),parsed->ruby.end(),
            [&](const auto& run){return run.first==index;});
        if(ruby_enabled && ruby!=parsed->ruby.end()) {
            if(ruby->reading.find_first_of(u"<\n")!=std::u16string::npos ||
               text.substr(index,ruby->count).find('\n')!=text.npos)
                return failure(PlainTextLayoutError::UnsupportedControlOrTag);
        }
        const bool is_opening=parameters.kinsoku && !kinsoku_remaining && member(opening,c);
        bool group_boundary=true,opening_pair=is_opening;
        size_t next=index+1;
        if(ascii_grouping) {
            if(!word_remaining) {
                const auto start=is_opening?next:index;const auto count=group_run(start);
                if(count) {
                    const auto span=measure(start,count);
                    if(is_opening) {
                        primary=pe64_proportional_group_width(true,result.cursor_x,plus(p.bounds_left,indent),primary,parts.trailing,span[2]);
                        word_remaining=count;next=start+count;
                    } else {
                        primary=pe64_proportional_group_width(false,result.cursor_x,plus(p.bounds_left,indent),primary,parts.trailing,span[2]);
                        word_remaining=count-1;next=start+count;
                    }
                    combined=primary;opening_pair=false;
                }
            } else {--word_remaining;group_boundary=false;opening_pair=false;}
        }
        if(ruby_enabled && ruby!=parsed->ruby.end()) {
            primary=combined=std::max(primary,measure(index,ruby->count)[2]);
            next=index+ruby->count;word_remaining=ruby->count-1;opening_pair=false;
        }
        if(parameters.kinsoku) {
            if(!kinsoku_remaining && group_boundary) {
                size_t count=0;while(next+count<items.size() && !tag_boundary(next+count) && member(prohibited_run,text[next+count]))++count;
                if(count && !(count==1 && quote_nonspace(text,next))) {
                    const auto span=measure(next,count);trailing=span[0];combined=plus(combined,span[2]);
                    size_t prefix=0;for(size_t j=0;j<count;++j)if(!member(line_start,text[next+j]))prefix=j+1;
                    if(prefix){const auto width=measure(next,prefix)[2];primary=plus(primary,width);trailing=minus(trailing,width);}
                    kinsoku_remaining=word_remaining+count;word_remaining=kinsoku_remaining;opening_pair=false;
                }
            } else if(kinsoku_remaining){--kinsoku_remaining;opening_pair=false;}
        }
        if(opening_pair && next<items.size() && !tag_boundary(next) && items[next])primary=plus(primary,measure(next,1)[0]);
        primary=plus(primary,parameters.style_wrap_extension);combined=plus(combined,parameters.style_wrap_extension);
        const bool fits=plus(result.cursor_x,primary)<=primary_limit && plus(result.cursor_x,combined)<=plus(p.bounds_right,1);
        bool wrap=!fits;
        if((fits && trailing>=1) || (!fits && member(line_start,c) && !quote_nonspace(text,index))) {
            const int32_t hanging_margin=parameters.allow_reserved_margin_for_punctuation?0:margin;
            wrap=plus(result.cursor_x,plus(advance,trailing))>plus(minus(p.bounds_right,hanging_margin),1);
        }
        if(wrap) {
            newline();
            if(plus(result.cursor_x,primary)<=primary_limit){word_remaining=0;kinsoku_remaining=0;}
        }
        if(p.trim_line_leading_space && after_break && c==space){after_break=false;continue;}
        if(ruby_enabled && ruby!=parsed->ruby.end())
            result.ruby_runs.push_back({result.nodes.size(),ruby->reading,
                plus(result.cursor_x,parts.leading),plus(result.cursor_y,p.baseline_offset),
                measure(index,ruby->count)[1],static_cast<uint32_t>(ruby->count),delay});
        const auto& glyph=item.glyph;const uint32_t left=proportional?glyph.metrics.ink_left():0U;
        const uint32_t width=proportional?std::min(glyph.width-left,
            (glyph.character_bitmap?glyph.metrics.ink_right()-left+1U:item.metric.ink_width)+right_extension):glyph.width;
        std::vector<uint8_t> full(size_t(glyph.width)*glyph.height*4U);
        GlyphBitmapView view{full,0,glyph.width*4U,glyph.width,glyph.height,2,4};
        if(!colorize_glyph_mask(view,glyph,p.color))return failure(PlainTextLayoutError::InvalidState);
        PlainTextGlyphNode node{c,plus(result.cursor_x,parts.leading),plus(result.cursor_y,p.baseline_offset),delay,0,p.fade_duration,0,width,glyph.height,std::vector<uint8_t>(size_t(width)*glyph.height*4U)};
        if(width)for(uint32_t y=0;y<glyph.height;++y)std::copy_n(full.data()+(size_t(y)*glyph.width+left)*4U,size_t(width)*4U,node.bitmap.data()+size_t(y)*width*4U);
        result.nodes.push_back(std::move(node));if(style_sources)style_sources->push_back(!proportional && !glyph.character_bitmap?GlyphMask{}:glyph);
        result.cursor_x=plus(result.cursor_x,advance);delay+=p.character_delay;after_break=false;
    }
    return {PlainTextLayoutError::None,std::move(result)};
}
PlainTextLayoutResult layout_pe64_proportional_text(std::u16string_view text,
    const FixedKinsokuLayoutParameters& parameters,const GlyphFontRequest& font,
    const GlyphOutlineProvider& provider,uint32_t reciprocal,uint32_t right_extension,
    std::vector<GlyphMask>* style_sources,bool ruby_enabled, std::span<const TextRubyAlias> aliases) {
    return layout_pe64_text(text,parameters,font,provider,reciprocal,right_extension,style_sources,true,ruby_enabled,aliases);
}
PlainTextLayoutResult layout_pe64_fixed_text(std::u16string_view text,
    const FixedKinsokuLayoutParameters& parameters,const GlyphFontRequest& font,
    const GlyphOutlineProvider& provider,bool ruby_enabled, std::span<const TextRubyAlias> aliases,
    std::vector<GlyphMask>* style_sources) {
    return layout_pe64_text(text,parameters,font,provider,0,0,style_sources,false,ruby_enabled,aliases);
}

static PlainTextLayoutResult layout_ruby_text(const PlainTextRubyRun& run,
    const Pe64RubyLayoutParameters& p,const GlyphFontRequest& font,
    const GlyphOutlineProvider& provider,bool legacy) {
    const auto failure=[](PlainTextLayoutError e){return PlainTextLayoutResult{e,std::nullopt};};
    if(run.reading.empty() || font.height<=0 || p.cell_width<=0 ||
       p.canvas_extension_x<0 || p.canvas_extension_y<0)
        return failure(PlainTextLayoutError::InvalidState);
    std::vector<GlyphMask> glyphs;
    std::vector<ProportionalGlyphMeasure> metrics;
    for(const char16_t c:run.reading) {
        if(c<0x20 || c=='<' || (c>=0xd800 && c<=0xdfff))
            return failure(PlainTextLayoutError::UnsupportedControlOrTag);
        auto glyph=load_glyph_outline(provider,font,c);
        if(!glyph)return failure(PlainTextLayoutError::MissingGlyph);
        if(glyph->metrics.ink_left()>glyph->width || glyph->metrics.ink_right()<glyph->metrics.ink_left())
            return failure(PlainTextLayoutError::InvalidState);
        metrics.push_back({{std::bit_cast<float>(glyph->metrics.words[7]),
            std::bit_cast<float>(glyph->metrics.words[8]),std::bit_cast<float>(glyph->metrics.words[9])},
            glyph->metrics.ink_right()-glyph->metrics.ink_left()+1U});
        glyphs.push_back(std::move(*glyph));
    }
    // 74B26 measures reading with proportional flag1. When it overflows
    // its base span it is centered; otherwise each glyph is centered in
    // an equal (baseWidth+1)/readingCount slot, retaining fractional slots.
    const auto measured=legacy?measure_legacy_proportional_fields(metrics,p.reciprocal,p.cell_width):
        measure_pe64_proportional_fields(metrics,p.font_spacing,p.reciprocal,p.cell_width);
    const bool overflow=run.width<measured[0];
    double cursor=plus(run.x,p.offset_x);
    if(overflow)cursor+=static_cast<int32_t>(uint32_t(run.width)-uint32_t(measured[0]))>>1;
    const double slot=double(plus(run.width,1))/double(glyphs.size());
    const auto snap=[](double x) {
        const auto lo=std::floor(x);const auto fraction=x-lo;
        constexpr double epsilon=0.00006103515625;
        if(fraction>=1.0-epsilon)return std::ceil(x);
        if(fraction<=epsilon)return lo;
        return x;
    };
    const int32_t quantum=static_cast<int32_t>(p.reciprocal*static_cast<uint32_t>(p.cell_width))>>16;
    const auto delay_slice=static_cast<uint32_t>(run.base_characters*p.character_delay)/glyphs.size();
    uint32_t delay=run.start_delay+static_cast<uint32_t>(delay_slice/2U);
    PlainTextLayout result{{},run.x,run.y,1};
    for(size_t i=0;i<glyphs.size();++i) {
        const auto& glyph=glyphs[i];const auto abc=metrics[i].abc;
        const double abc_width=double(float(float(abc[0]+abc[1])+abc[2]));
        double advance;
        int32_t x;
        if(overflow) {
            x=static_cast<int32_t>(snap(cursor));
            if(p.reciprocal==0)
                advance=int32_t(abc[0]+0.5F)+int32_t(abc[1]+0.5F)+int32_t(abc[2]+0.5F);
            else advance=metrics[i].ink_width+quantum;
        } else {
            x=static_cast<int32_t>(snap(cursor+(slot-abc_width)*0.5+0.5));
            advance=slot;
        }
        // 140074E34 adds the image double at 14018D5E8 (1.5).
        const auto canvas_width=static_cast<int32_t>(snap(abc_width+1.5))+p.canvas_extension_x;
        const int32_t canvas_height=font.height+p.canvas_extension_y;
        if(canvas_width<0 || canvas_height<0 || uint64_t(canvas_width)*uint64_t(canvas_height)>SIZE_MAX/4U)
            return failure(PlainTextLayoutError::InvalidState);
        PlainTextGlyphNode node{run.reading[i],x,
            minus(plus(run.y,p.offset_y),font.height),delay,0,p.fade_duration,2,
            static_cast<uint32_t>(canvas_width),static_cast<uint32_t>(canvas_height),
            std::vector<uint8_t>(size_t(canvas_width)*canvas_height*4U)};
        GlyphBitmapView view{node.bitmap,0,node.width*4U,node.width,node.height,2,4};
        if(!colorize_glyph_mask(view,glyph,p.color))return failure(PlainTextLayoutError::InvalidState);
        result.nodes.push_back(std::move(node));cursor+=advance;delay+=static_cast<uint32_t>(delay_slice);
    }
    return {PlainTextLayoutError::None,std::move(result)};
}

PlainTextLayoutResult layout_pe64_ruby_text(const PlainTextRubyRun& run,
    const Pe64RubyLayoutParameters& p,const GlyphFontRequest& font,const GlyphOutlineProvider& provider) {
    return layout_ruby_text(run,p,font,provider,false);
}
PlainTextLayoutResult layout_legacy_ruby_text(const PlainTextRubyRun& run,
    const Pe64RubyLayoutParameters& p,const GlyphFontRequest& font,const GlyphOutlineProvider& provider) {
    return layout_ruby_text(run,p,font,provider,true);
}
} // namespace bgiplayer
