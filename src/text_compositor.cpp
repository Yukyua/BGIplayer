#include "bgiplayer/vm.hpp"
#include "bgiplayer/glyph_style.hpp"
#include "bgiplayer/text_measure.hpp"
#include "bgiplayer/gpu_pixel_kernels.hpp"
#include <bit>
#include <iostream>

namespace bgiplayer {
namespace {
std::u16string target_utf16(std::string_view text, TextEncoding encoding) {
    std::u16string output;
    for (const uint32_t c : decode_text_code_points(text, encoding)) {
        if (c <= 0xFFFFU) output.push_back(static_cast<char16_t>(c));
        else {
            output.push_back(static_cast<char16_t>(0xD800U + ((c - 0x10000U) >> 10U)));
            output.push_back(static_cast<char16_t>(0xDC00U + ((c - 0x10000U) & 0x3FFU)));
        }
    }
    return output;
}
std::vector<TextRubyAlias> window_ruby_dictionary(const std::map<std::string,std::string>& source) {
    std::vector<TextRubyAlias> aliases;
    for(const auto& [base,reading]:source) {
        aliases.push_back({target_utf16(base,detect_text_encoding(base)),
            target_utf16(reading,detect_text_encoding(reading))});
    }
    std::stable_sort(aliases.begin(),aliases.end(),[](const auto& a,const auto& b) {
        return a.base.size()>b.base.size();
    });
    return aliases;
}
float text_font_spacing(bool pe64,const std::map<std::string,std::array<uint32_t,5>>& tuples,
    const std::string& face,uint32_t height) {
    if(!pe64)return 0;
    const auto found=tuples.find(face);if(found==tuples.end())return 0;
    const auto& t=found->second;
    const bool identity=t[0]==0 && t[1]==0 && t[2]==0 && t[3]==0;
    if(!identity) {
        // Native 0E accepts transform tuples before a font is acquired. The
        // portable glyph provider still rasterizes its untransformed face;
        // retain this visible gap instead of claiming the numeric spacing
        // implementation also implements the first four pixel transforms.
        static std::set<std::array<uint32_t,6>> reported;
        if(reported.insert({height,t[0],t[1],t[2],t[3],t[4]}).second) {
            std::cerr<<"warning: TEXT_FONT_TRANSFORM_UNSUPPORTED reason=host_glyph_transform_not_implemented"
                <<" font_height="<<height<<" tuple="<<std::hex;
            for(auto word:t)std::cerr<<word<<',';
            std::cerr<<std::dec<<" fallback=untransformed_host_glyphs_with_original_spacing\n";
        }
    }
    return pe64_font_spacing(static_cast<int32_t>(height),identity?65536U:t[1],static_cast<int32_t>(t[4]));
}
uint32_t cache_paragraph_line_heights(std::map<uint32_t,std::vector<uint32_t>>& cache,
    uint32_t& counter,uint32_t lines,uint32_t line_advance) {
    // 140078280 / 0043A9A0 assigns an increasing id and retains the
    // newest sixteen line-height vectors. Consuming a vector keeps its id
    // retired: the counter is independent of the live entries.
    const uint32_t id=++counter;
    cache[id]=std::vector<uint32_t>(lines,line_advance);
    if(cache.size()>16U) {
        auto oldest=cache.begin();
        for(auto entry=cache.begin();entry!=cache.end();++entry)
            if(uint32_t(id-entry->first)>uint32_t(id-oldest->first))oldest=entry;
        cache.erase(oldest);
    }
    return id;
}
}

bool VM::apply_original_text_style(PlainTextLayout& layout, uint32_t style,
    int32_t style_x, int32_t style_y, uint32_t effect_color, uint32_t effect_parameter,
    std::span<const GlyphMask> style_sources,uint32_t private_cache_width,
    uint32_t private_cache_height,bool proportional) {
    if (style == 0) {
        if(!compact_64_opcodes_)return true;
        // 77D2B copies the main temporary into the final node, even without
        // a style. Private color/alpha payloads must retain that second copy.
        uint32_t temporary=0xFFFFFFF0U;
        while(graphics_surfaces_.contains(temporary)||graphics_surfaces_.contains(temporary-1U))temporary-=2U;
        const size_t history=bitmap_blits_.size();const bool dirty=display_dirty_;
        const auto cleanup=[&]{graphics_surfaces_.erase(temporary);graphics_surfaces_.erase(temporary-1U);
            bitmap_blits_.resize(history);display_dirty_=dirty;};
        try{for(auto& node:layout.nodes) {
            if(node.codepoint<0xF001U || node.codepoint>0xF7FFU || !node.width || !node.height)continue;
            GraphicsSurface main;main.width=node.width;main.height=node.height;main.format=2;
            main.bytes_per_pixel=4;main.stride=node.width*4U;main.pixels=node.bitmap;
            graphics_surfaces_[temporary-1U]=main;main.pixels.assign(main.pixels.size(),0);
            graphics_surfaces_[temporary]=std::move(main);
            if(!record_bitmap_blit(temporary,temporary-1U,0,0,0,0))throw BpFault("private final glyph copy failed");
            node.bitmap=std::move(graphics_surfaces_.at(temporary).pixels);
        }cleanup();}catch(...){cleanup();throw;}
        return true;
    }
    if (style > 2 || (style != 0 && (style_x < 1 || style_y < 1))) return false;
    if(!style_sources.empty() && style_sources.size()!=layout.nodes.size())return false;
    size_t glyph_index=0;
        // 0043D4xx keeps each previous node's unstyled main bitmap at+3C.
        // Only that saved main alpha removes overlap from the next outline.
        uint32_t scratch=0xFFFFFFF0U;
        while(graphics_surfaces_.contains(scratch) || graphics_surfaces_.contains(scratch-1U) ||
              graphics_surfaces_.contains(scratch-2U))scratch-=3U;
        const std::size_t blit_count=bitmap_blits_.size();
        const auto cleanup=[&](){for(uint32_t i=0;i<3;++i)graphics_surfaces_.erase(scratch-i);
            bitmap_blits_.resize(blit_count);};
        const auto surface=[](uint32_t w,uint32_t h,std::vector<uint8_t> pixels){
            GraphicsSurface out;out.width=w;out.height=h;out.format=2;out.bytes_per_pixel=4;
            out.stride=w*4U;out.pixels=std::move(pixels);return out;};
        std::optional<PlainTextGlyphNode> previous;
        try{
            for(auto& node:layout.nodes){
                PlainTextGlyphNode main=node;
                const uint32_t dx=static_cast<uint32_t>(style_x),dy=static_cast<uint32_t>(style_y);
                // Final node+28 keeps the main temporary's cropped view. A
                // private cache has its own square/slot canvas plus style pad;
                // the outline temporary separately uses the font-cache canvas.
                const uint32_t width=node.width,height=node.height;
                graphics_surfaces_[scratch]=surface(width,height,std::vector<uint8_t>(static_cast<std::size_t>(width)*height*4U));
                graphics_surfaces_[scratch-1U]=surface(main.width,main.height,main.bitmap);


                const GlyphMask* original = style==2 && !style_sources.empty() &&
                    (style_sources[glyph_index].width!=0 || style_sources[glyph_index].character_bitmap)
                    ?&style_sources[glyph_index]:nullptr;
                const bool private_canvas=compact_64_opcodes_ && original && original->character_bitmap && private_cache_width!=UINT32_MAX;
                const uint32_t ew=(private_canvas?private_cache_width:original?original->width:main.width)+(style==2?dx*2U:0U);
                const uint32_t eh=(private_canvas?private_cache_height:original?original->height:main.height)+(style==2?dy*2U:0U);
                std::vector<uint8_t> effect(static_cast<std::size_t>(ew)*eh*4U);
                GlyphBitmapView effect_view{effect,0,ew*4U,ew,eh,2,4};
                bool valid=false;
                if(style==1){GlyphBitmapView main_view{main.bitmap,0,main.width*4U,main.width,main.height,2,4};
                    valid=colorize_glyph_shadow(effect_view,main_view,effect_color);}
                else{
                    GlyphMask mask{main.width,main.height,main.width,{}, {}};
                    mask.alpha.resize(static_cast<std::size_t>(main.width)*main.height);
                    for(std::size_t pixel=0;pixel<mask.alpha.size();++pixel)mask.alpha[pixel]=main.bitmap[pixel*4U+3U];
                    const auto& input=original?*original:mask;
                    // FreeType supplies normal font-cache glyphs. PE64 72ED1→3F480
                    // uses the original weighted kernel; gaiji's bitmap-cache
                    // boxsum branch is a distinct source and is not selected here.
                    if(input.character_bitmap) {
                        // 72AE0 refuses color caches; only format3 has the
                        // gaiji rectangular-alpha outline arm.
                        auto alpha=input;
                        alpha.width=input.metrics.ink_right()+1U;alpha.height=input.metrics.words[6]+1U;
                        valid=input.source_format!=3 || colorize_pe64_gaiji_outline(effect_view,alpha,dx,dy,effect_color);
                    } else valid=colorize_glyph_outline(effect_view,input,dx,dy,effect_color);
                }
                if(!valid){cleanup();return false;}
                uint32_t effect_width=ew;
                if(original && (!original->character_bitmap || !compact_64_opcodes_ || proportional)) {
                    // 140077BA0 outlines the complete cache mask, then 77BDF
                    // crops its view using the same horizontally extended ink rect.
                    const uint32_t left=original->metrics.ink_left();
                    if(left>ew){cleanup();return false;}
                    effect_width=std::min(ew-left,original->metrics.ink_right()-left+1U+dx*2U);
                    std::vector<uint8_t> cropped(size_t(effect_width)*eh*4U);
                    for(uint32_t y=0;y<eh;++y)std::copy_n(effect.data()+(size_t(y)*ew+left)*4U,
                        size_t(effect_width)*4U,cropped.data()+size_t(y)*effect_width*4U);
                    effect=std::move(cropped);
                }
                graphics_surfaces_[scratch-2U]=surface(effect_width,eh,std::move(effect));
                if(style==2 && previous){
                    graphics_surfaces_[scratch-1U]=surface(previous->width,previous->height,previous->bitmap);
                    if(!record_bitmap_blit(scratch-2U,scratch-1U,previous->x-node.x+style_x,
                        previous->y-node.y+style_y,7,0x100U))throw BpFault("rich previous-main outline subtraction failed");
                    graphics_surfaces_[scratch-1U]=surface(main.width,main.height,main.bitmap);
                }
                if(!record_bitmap_blit(scratch,scratch-2U,style==1?style_x:0,style==1?style_y:0,1,effect_parameter) ||
                   !record_bitmap_blit(scratch,scratch-1U,style==2?style_x:0,style==2?style_y:0,0,0))
                    throw BpFault("rich styled glyph composition failed");
                node.width=width;node.height=height;node.bitmap=std::move(graphics_surfaces_.at(scratch).pixels);
                previous=std::move(main);++glyph_index;
            }
            cleanup();
        }catch(...){cleanup();throw;}
    return true;
}

// 004902D0 -> 00404260 writes the third 0043A650 metric, not a font id.
uint32_t VM::create_text_measure(uint32_t output, std::span<const uint32_t> o, std::string text) {
    if(o.size()!=7 || output==0)return 0x80000001U;
    const auto status=register_font_cache_entry(o[2],o[3],o[4],o[5],0,0);
    if(status!=0)return status==0x80000004U?0x80000003U:status==0x80000003U?0x80000002U:0x80000001U;

    int32_t spacing=0;
    const auto face=font_name(o[2]);
    auto font=std::find_if(font_cache_registrations_.begin(),font_cache_registrations_.end(),[&](const auto& f){
        return f.face_name==face && f.parameter_2==o[3] && f.parameter_3==o[4] && f.parameter_4==o[5];});
    spacing=text_integer_font_spacing(o[6],o[5],font!=font_cache_registrations_.end()?font->first_init:0U,
        font!=font_cache_registrations_.end()?font->second_init:0U,text_metrics_[2]);
    const auto decoded=target_utf16(text,text_encoding_mode_==1?TextEncoding::Utf8:detect_text_encoding(text));
    const int32_t cell=static_cast<int32_t>(o[3]*o[4]/100U);
    const float advance_gap=text_font_spacing(compact_64_opcodes_,scaled_font_tuples_,face,o[3]);
    uint32_t width=0;
    if(o[6]!=0) {
        if(!window_glyph_provider_factory_)throw BpFault("proportional measurement glyph provider unavailable");
        WindowGlyphRequest request;
        request.face_name=face;request.logical_height=o[3];request.width_percent=o[4];
        request.bold=o[5];request.proportional=o[6];
        request.font.face=target_utf16(face,detect_text_encoding(face));
        request.font.height=static_cast<int32_t>(o[3]);request.font.weight=o[5]?700:100;
        request.font.quality=2;request.font.outline_gray_bits=6;
        auto provider=window_glyph_provider_factory_(request);
        if(!provider)throw BpFault("proportional measurement explicit font could not be opened");
        // Nonzero transformed-font spacing requires the separate cache geometry.
        if(auto tuple=scaled_font_tuples_.find(face);!compact_64_opcodes_ && tuple!=scaled_font_tuples_.end() && tuple->second[4]!=0)
            throw BpFault("proportional measurement transformed font spacing unsupported");
        std::vector<ProportionalGlyphMeasure> glyphs;
        for(char16_t c:decoded) {
            if(c==0)break;
            if(c>=0xF001U && c<=0xF7FFU) {
                // 1400797E2 / 0043A7C2 ask only the slot width getter.
                // Measuring never decodes, colorizes or scales its pixels.
                const uint32_t key=compact_64_opcodes_?uint32_t(c)|0x80000000U:
                    0x8000F000U|(uint32_t(c)&0xFFU);
                const auto slot=graphics_surfaces_.find(key);
                glyphs.push_back({{},slot==graphics_surfaces_.end()?0U:slot->second.width,true});
                continue;
            }
            auto glyph=(*provider)(request.font,static_cast<uint32_t>(c));
            if(!glyph)throw BpFault("proportional measurement host glyph unavailable");
            GlyphBitmapView view{{},0,glyph->stride,glyph->width,glyph->height,2,4};
            trim_glyph_bitmap_view(view,glyph->metrics);
            glyphs.push_back({{std::bit_cast<float>(glyph->metrics.words[7]),
                std::bit_cast<float>(glyph->metrics.words[8]),std::bit_cast<float>(glyph->metrics.words[9])},glyph->character_bitmap?glyph->character_bitmap_source_width:view.width,glyph->character_bitmap});
        }
        width=static_cast<uint32_t>(compact_64_opcodes_
            ? measure_pe64_proportional(glyphs,advance_gap,graphics_reciprocal_,cell)
            : measure_legacy_proportional_fields(glyphs,graphics_reciprocal_,cell)[2]);
    } else {
        for(char16_t c:decoded){if(c==0)break;
            if(c>=0xF001U && c<=0xF7FFU) {
                // PE32 00438380 indexes its descriptor array by the low byte;
                // PE64 140073340 looks up the complete tagged Unicode value.
                const uint32_t key=compact_64_opcodes_ ? uint32_t(c)|0x80000000U
                    : 0x8000F000U|(uint32_t(c)&0xFFU);
                const auto slot=graphics_surfaces_.find(key);
                width+=(slot==graphics_surfaces_.end()?0U:slot->second.width)+static_cast<uint32_t>(spacing);
            } else width+=static_cast<uint32_t>(target_wide_character(c)?cell:cell/2)+static_cast<uint32_t>(spacing);}
        width-=static_cast<uint32_t>(spacing);
    }
    memory_.write_u32(output,0,width);
    TextMeasureState state;state.operands.assign(o.begin(),o.end());state.font_id=o[2];
    state.text=std::move(text);state.mode=o[6];state.handle=width;
    text_measure_states_.push_back(std::move(state));
    return 0;
}

// 00492090 -> 004041C0 -> 0043A2C0/0043A370: synchronous bitmap text.
// The wrapper's top dword is discarded. The returned value is the parser's
// line count (or line-cache identity when 00532844 is enabled), not an object.
uint32_t VM::create_rich_text_object(uint8_t family, uint8_t subopcode,
    std::span<const uint32_t> o, std::string text, std::string auxiliary) {
    if(o.size()<10 || !graphics_surfaces_.contains(o[0]))return 0x80000004U;
    // Independent PE32 00490340/00490510 and PE64 1400E01B0/1400DFF00
    // wrappers share the bitmap frontend but place color last and use the
    // global style constructor. 9D's extra flag selects a zero style.
    if(family==3 && ((subopcode==0x9C && o.size()==14) ||
                     (subopcode==0x9D && o.size()==15))) {
        const bool default_style=subopcode==0x9C || o[14]==0;
        const std::array<uint32_t,21> normalized{o[0],o[1],o[2],o[3],o[13],o[4],o[5],o[13],
            o[6],o[7],o[8],o[9],o[10],o[11],o[12],
            default_style?grp0_render_global_529bbc_:0U,
            default_style?grp0_render_global_529bc0_:0U,
            default_style?grp0_render_global_529bc4_:0U,
            default_style?grp0_render_global_529bcc_:0U,
            default_style?grp0_render_global_529bc8_:0U,0};
        const auto status=create_rich_text_object(4,0x9C,normalized,std::move(text),std::move(auxiliary));
        if(status<0x80000000U && !rich_text_object_states_.empty()) {
            auto& state=rich_text_object_states_.back();
            state.family=family;state.subopcode=subopcode;state.operands.assign(o.begin(),o.end());
        }
        return status;
    }
    if(family!=4 || subopcode!=0x9C || o.size()!=21)
        throw BpFault("bitmap text frontend ABI not yet recovered");
    const auto unsupported=[&](const char* why)->uint32_t{
        std::ostringstream detail;detail<<why<<" tuple="<<std::hex;
        for(auto word:o)detail<<word<<',';
        detail<<" text_bytes=";for(std::size_t i=0;i<std::min<std::size_t>(text.size(),128);++i)
            detail<<static_cast<uint32_t>(static_cast<uint8_t>(text[i]))<<',';
        if(o.size()>8)detail<<" font="<<font_name(o[8]);
        throw BpFault(detail.str());
    };
    std::vector<TextRubyAlias> aliases;
    if(o[6]!=0 && !auxiliary.empty()) {
        auto dictionary=parse_text_ruby_dictionary(target_utf16(auxiliary,
            text_encoding_mode_==1?TextEncoding::Utf8:detect_text_encoding(auxiliary)));
        if(!dictionary)return unsupported("bitmap text malformed ruby dictionary");
        aliases=std::move(*dictionary);
    }
    const uint32_t font_id=o[8],height=o[9],scale=o[10],bold=o[11];
    const uint32_t status=register_font_cache_entry(font_id,height,scale,bold,0,0);
    if(status!=0)return status==0x80000004U?0x80000003U:status==0x80000003U?0x80000002U:0x80000001U;
    if(!window_glyph_provider_factory_)return unsupported("bitmap text host glyph provider unavailable");
    WindowGlyphRequest request;
    request.face_name=font_name(font_id);request.logical_height=height;
    request.width_percent=scale;request.bold=bold;request.proportional=o[12];
    request.font.face=target_utf16(request.face_name,detect_text_encoding(request.face_name));
    request.font.height=static_cast<int32_t>(height);request.font.weight=bold==0?100:700;
    request.font.quality=2;request.font.outline_gray_bits=6;
    auto provider=window_glyph_provider_factory_(request);
    if(!provider)return unsupported("bitmap text font could not be opened");
    auto decoded=target_utf16(text,text_encoding_mode_==1?TextEncoding::Utf8:detect_text_encoding(text));
    bool grouping=true;
    if(!decoded.empty() && decoded.front()==0x0B){grouping=false;decoded.erase(decoded.begin());}
    const auto setting=[&](uint32_t k){auto i=graphics_settings_.find(k);return i==graphics_settings_.end()?0U:i->second;};
    const auto& target=graphics_surfaces_.at(o[0]);
    uint32_t style=o[15];
    if(style>2 || (style!=0 && (o[16]>100 || o[17]>100 || o[19]>256)))
        return unsupported("bitmap text invalid style tuple");
    const int32_t dx=style?std::max(1,static_cast<int32_t>(height*o[16]/100U)):0;
    const int32_t dy=style?std::max(1,static_cast<int32_t>(height*o[17]/100U)):0;
    *provider=with_character_bitmaps(std::move(*provider),o[4],height,scale,compact_64_opcodes_?uint32_t(dx)*(style==2?2U:1U):0U,compact_64_opcodes_?uint32_t(dy)*(style==2?2U:1U):0U);
    int32_t spacing=0;
    auto cache=std::find_if(font_cache_registrations_.begin(),font_cache_registrations_.end(),[&](const auto& e){
        return e.face_name==request.face_name && e.parameter_2==height && e.parameter_3==scale && e.parameter_4==bold;});
    if(cache!=font_cache_registrations_.end())spacing=text_integer_font_spacing(0,bold,
        cache->first_init,cache->second_init,0);
    int32_t baseline=o[5]?std::max(4,static_cast<int32_t>(grp1_text_style_.field_5328bc)>0?
        static_cast<int32_t>(grp1_text_style_.field_5328bc):static_cast<int32_t>(height*text_metrics_[3]/100U)):0;
    if(o[5] && setting(0x80000002U))baseline-=static_cast<int32_t>(grp1_text_style_.field_53283c);
    if(style==2 && setting(0x80000002U))baseline+=dy;
    PlainTextLayoutParameters p{static_cast<int32_t>(o[1]),static_cast<int32_t>(o[2]),0,
        static_cast<int32_t>(target.width)-1,static_cast<int32_t>(height*scale/100U),
        static_cast<int32_t>(text_metrics_[2])+spacing,
        static_cast<int32_t>(height+height*o[14]/100U),baseline,
        text_metrics_[0],text_metrics_[1]?text_metrics_[1]:1U,o[4],setting(0x80000004U)!=0,grouping};
    p.font_spacing=text_font_spacing(compact_64_opcodes_,scaled_font_tuples_,request.face_name,height);
    FixedKinsokuLayoutParameters fixed{p,static_cast<int32_t>(height),static_cast<int32_t>(height),
        o[5]?static_cast<int32_t>(text_metrics_[4]):0,o[13]!=0,true,text_metrics_[5]!=0,dx};
    if(o[12]!=0) {
        if(auto tuple=scaled_font_tuples_.find(request.face_name);
           !compact_64_opcodes_ && tuple!=scaled_font_tuples_.end() && tuple->second[4]!=0)
            return unsupported("bitmap proportional transformed font spacing unsupported");
    }
    std::vector<GlyphMask> full_glyphs;
    auto result=!compact_64_opcodes_ ? layout_legacy_text(decoded,fixed,request.font,*provider,
            o[12]!=0,graphics_reciprocal_,style==2?uint32_t(dx)*2U:uint32_t(dx),
            style==2?&full_glyphs:nullptr,o[5]!=0,aliases)
        : o[12]!=0 ? layout_pe64_proportional_text(decoded,fixed,request.font,*provider,
            graphics_reciprocal_,style==2?uint32_t(dx)*2U:uint32_t(dx),style==2?&full_glyphs:nullptr,o[5]!=0,aliases)
        : compact_64_opcodes_ ? layout_pe64_fixed_text(decoded,fixed,request.font,*provider,o[5]!=0,aliases,style==2?&full_glyphs:nullptr)
        : layout_plain_fixed_kinsoku_text(decoded,fixed,request.font,*provider);
    if(!result.layout){
        const auto why=result.error==PlainTextLayoutError::UnsupportedControlOrTag?
            "bitmap text unsupported control/tag":result.error==PlainTextLayoutError::MissingGlyph?
            "bitmap text missing glyph":"bitmap text invalid layout/canvas state";
        return unsupported(why);
    }
    if(!apply_original_text_style(*result.layout,style,dx,dy,o[18],256U-o[19],full_glyphs,height*scale*2U/100U,height*3U/2U,o[12]!=0))
        return unsupported("bitmap text style branch unsupported");
    if(!apply_original_text_ruby(*result.layout,request,o[7],style,o[16],o[17],o[18],256U-o[19]))
        return unsupported("bitmap auxiliary ruby layout unsupported");
    uint32_t scratch=0xFFFFFFF0U;
    while(graphics_surfaces_.contains(scratch))--scratch;
    const auto count=bitmap_blits_.size();
    try{
        for(const auto& node:result.layout->nodes){
            GraphicsSurface glyph;glyph.width=node.width;glyph.height=node.height;glyph.format=2;
            glyph.bytes_per_pixel=4;glyph.stride=node.width*4;glyph.pixels=node.bitmap;
            graphics_surfaces_[scratch]=std::move(glyph);
            if(!record_bitmap_blit(o[0],scratch,node.x,node.y,0,0))
                throw BpFault("bitmap text final glyph copy unsupported");
        }
    }catch(...){graphics_surfaces_.erase(scratch);bitmap_blits_.resize(count);throw;}
    graphics_surfaces_.erase(scratch);bitmap_blits_.resize(count);
    const uint32_t output=setting(0x80000009U)!=0
        ? cache_paragraph_line_heights(render_cache_records_,next_rich_text_object_id_,
            result.layout->lines,static_cast<uint32_t>(p.line_advance))
        : result.layout->lines;
    RichTextObjectState state;state.family=family;state.subopcode=subopcode;
    state.operands.assign(o.begin(),o.end());state.target=o[0];state.text=std::move(text);
    state.handle=output;
    rich_text_object_states_.push_back(std::move(state));
    display_dirty_=true;
    return output;
}

// 140075140 acquires the ruby font, creates kind2 reading chains through
// 140074A50 and links each chain after its base anchor. Main cursor and bounds
// come exclusively from the main parser; the metadata is consumed before a
// timed procedure (and its serializable layout) is installed.
bool VM::apply_original_text_ruby(PlainTextLayout& layout, const WindowGlyphRequest& main_font,
    uint32_t color, uint32_t style, uint32_t percent_x, uint32_t percent_y,
    uint32_t effect_color, uint32_t effect_parameter) {
    if (layout.ruby_runs.empty()) return true;
    if (!window_glyph_provider_factory_) return false;
    WindowGlyphRequest request=main_font;
    if (!grp1_text_style_.font_name.empty()) request.face_name=grp1_text_style_.font_name;
    request.logical_height=static_cast<uint32_t>(std::max(4,
        static_cast<int32_t>(grp1_text_style_.field_5328bc)>0 ?
        static_cast<int32_t>(grp1_text_style_.field_5328bc) :
        static_cast<int32_t>(main_font.logical_height*text_metrics_[3]/100U)));
    if (static_cast<int32_t>(grp1_text_style_.field_533218)>0)
        request.width_percent=grp1_text_style_.field_533218;
    request.font.face=target_utf16(request.face_name,detect_text_encoding(request.face_name));
    request.font.height=static_cast<int32_t>(request.logical_height);
    request.proportional=1;
    if (auto tuple=scaled_font_tuples_.find(request.face_name);
        !compact_64_opcodes_ && tuple!=scaled_font_tuples_.end() && tuple->second[4]!=0) return false;
    auto provider=window_glyph_provider_factory_(request);
    if (!provider) return false;
    if (grp1_text_style_.field_529bf8!=0xFFFFFFFFU) color=grp1_text_style_.field_529bf8;
    if (grp1_text_style_.field_529bfc!=0xFFFFFFFFU)
        effect_parameter=256U-grp1_text_style_.field_529bfc;
    const int32_t dx=style?std::max(1,static_cast<int32_t>(request.logical_height*percent_x/100U)):0;
    const int32_t dy=style?std::max(1,static_cast<int32_t>(request.logical_height*percent_y/100U)):0;
    Pe64RubyLayoutParameters parameters{
        static_cast<int32_t>(request.logical_height*request.width_percent/100U),
        graphics_reciprocal_,text_font_spacing(compact_64_opcodes_,scaled_font_tuples_,request.face_name,request.logical_height),static_cast<int32_t>(grp1_text_style_.field_532838)-(style==2?dx:0),
        static_cast<int32_t>(grp1_text_style_.field_53283c)-(style==2?dy:0),style==2?dx*2:dx,
        style==2?dy*2:dy,color,text_metrics_[0],text_metrics_[1]==0?1U:text_metrics_[1]};
    for (auto run=layout.ruby_runs.rbegin();run!=layout.ruby_runs.rend();++run) {
        auto reading=compact_64_opcodes_?layout_pe64_ruby_text(*run,parameters,request.font,*provider)
            :layout_legacy_ruby_text(*run,parameters,request.font,*provider);
        if (!reading.layout || !apply_original_text_style(*reading.layout,style,dx,dy,
            effect_color,effect_parameter)) return false;
        const auto at=layout.nodes.begin()+static_cast<std::ptrdiff_t>(run->first_node+1U);
        layout.nodes.insert(at,std::make_move_iterator(reading.layout->nodes.begin()),
            std::make_move_iterator(reading.layout->nodes.end()));
    }
    layout.ruby_runs.clear();
    return true;
}

uint32_t VM::draw_direct_text(uint8_t family, uint8_t subopcode,
                              std::span<const uint32_t> operands, std::string text) {
    if (!debug_enabled()) direct_text_draw_states_.clear();
    if (operands.empty()) return 1;
    const uint32_t window = operands[0];
    const auto object = window_objects_.find(window);
    if (object == window_objects_.end()) return 0xFFFFFFFFU; // 0046D720 mapping.
    DirectTextDrawState state;
    state.family = family;
    state.subopcode = subopcode;
    if (debug_enabled()) state.operands.assign(operands.begin(), operands.end());
    state.window = window;
    state.text = std::move(text);
    const auto failure = [&](std::string_view reason) {
        state.unsupported_reason = reason;
        direct_text_draw_states_.push_back(std::move(state));
        return 1U;
    };
    const std::size_t count = family == 4U ? 11U : subopcode == 0x93U ? 6U : 5U;
    if ((family != 3U && family != 4U) ||
        (subopcode != 0x91U && !(family == 3U && subopcode == 0x93U)) || operands.size() != count) {
        return failure("unsupported direct-text ABI");
    }
    const auto& target = object->second;
    if (!window_glyph_provider_factory_) return failure("window glyph provider unavailable");
    if (target.font_name.empty() || target.font_size == 0 || target.font_scale == 0) {
        return failure("window font unavailable");
    }
    if (target.text_boundary_mode_398 != 0 || target.text_field_39c != 0) {
        return failure("vertical or aligned text unsupported direction=" +
            std::to_string(target.text_boundary_mode_398) + " alignment=" +
            std::to_string(target.text_field_39c));
    }
    const bool proportional = target.font_field5 != 0;
    if (proportional || compact_64_opcodes_) {
        if (auto tuple = scaled_font_tuples_.find(target.font_name);
            !compact_64_opcodes_ && tuple != scaled_font_tuples_.end() && tuple->second[4] != 0)
            return failure("direct transformed font spacing unsupported");
    }
    const uint32_t ruby = family == 4U ? operands[3] : operands[subopcode == 0x93U ? 4U : 3U];
    const uint32_t kinsoku = family == 4U ? operands[5] : operands[subopcode == 0x93U ? 5U : 4U];
    const uint32_t line_advance = target.font_size + (target.font_size * target.line_spacing_percent) / 100U;
    const bool default_style = family == 3U && (subopcode != 0x93U || operands[3] == 0);
    const uint32_t style=family==4U?operands[6]:default_style?grp0_render_global_529bbc_:0U;
    const auto style_percent_x=family==4U?operands[7]:grp0_render_global_529bc0_;
    const auto style_percent_y=family==4U?operands[8]:grp0_render_global_529bc4_;
    const auto style_opacity=family==4U?operands[10]:grp0_render_global_529bc8_;
    const auto style_color=family==4U?operands[9]:grp0_render_global_529bcc_;
    if(style>2 || (style!=0 && (style_percent_x>100 || style_percent_y>100 || style_opacity>256)))
        return failure("invalid direct style tuple");
    const int32_t style_x=style?std::max(1,static_cast<int32_t>(target.font_size*style_percent_x/100U)):0;
    const int32_t style_y=style?std::max(1,static_cast<int32_t>(target.font_size*style_percent_y/100U)):0;
    WindowGlyphRequest request;
    request.window = window;
    request.face_name = target.font_name;
    request.logical_height = target.font_size;
    request.width_percent = target.font_scale;
    request.bold = target.font_field4;
    request.proportional = target.font_field5;
    request.font_field6 = target.font_field6;
    request.font.face = target_utf16(request.face_name, detect_text_encoding(request.face_name));
    request.font.height = static_cast<int32_t>(target.font_size);
    request.font.weight = target.font_field4 == 0 ? 100 : 700;
    request.font.quality = 2;
    request.font.outline_gray_bits = 6;
    auto provider = window_glyph_provider_factory_(request);
    if (!provider) return failure("explicit window font could not be opened");
    std::u16string decoded;
    try {
        decoded = target_utf16(state.text, text_encoding_mode_ == 1 ? TextEncoding::Utf8 : detect_text_encoding(state.text));
    } catch (const std::exception&) {
        return failure("invalid target text encoding");
    }
    // 140068DA0 / 14007818A pass the process-global alias dictionary.
    const auto aliases=ruby!=0?window_ruby_dictionary(rich_text_aliases_):std::vector<TextRubyAlias>{};
    bool word_grouping = true;
    if (!decoded.empty() && decoded.front() == 0x0B) {
        word_grouping = false;
        decoded.erase(decoded.begin());
    }
    int32_t font_spacing = 0;
    const auto registered = std::find_if(font_cache_registrations_.begin(), font_cache_registrations_.end(),
        [&](const auto& entry) {
            return entry.face_name == target.font_name && entry.parameter_2 == target.font_size &&
                   entry.parameter_3 == target.font_scale && entry.parameter_4 == target.font_field4;
        });
    if (registered != font_cache_registrations_.end()) {
        font_spacing = text_integer_font_spacing(0,target.font_field4,
            registered->first_init,registered->second_init,0);
    }
    PlainTextLayoutParameters parameters{
        target.text_cursor_x_390, target.text_cursor_y_394, target.update_left, target.update_right,
        static_cast<int32_t>((target.font_size * target.font_scale) / 100U),
        static_cast<int32_t>(text_metrics_[2] + static_cast<uint32_t>(font_spacing)),
        static_cast<int32_t>(line_advance), 0,
        text_metrics_[0], text_metrics_[1] == 0 ? 1U : text_metrics_[1], operands[2],
        false, word_grouping};
    // PE64 140068DA0 forwards window font/cursor/bounds to 1400792E0,
    // the same parser used by bitmap text. Its glyph placement is shared.
    const auto setting=[&](uint32_t key){auto it=graphics_settings_.find(key);return it==graphics_settings_.end()?0U:it->second;};
    if (ruby != 0) {
        parameters.baseline_offset = std::max(4,static_cast<int32_t>(grp1_text_style_.field_5328bc)>0 ?
            static_cast<int32_t>(grp1_text_style_.field_5328bc) : static_cast<int32_t>(target.font_size*text_metrics_[3]/100U));
        if (setting(0x80000002U) != 0)
            parameters.baseline_offset -= static_cast<int32_t>(grp1_text_style_.field_53283c);
    }
    if(style==2 && setting(0x80000002U)!=0)parameters.baseline_offset+=style_y;
    parameters.font_spacing=text_font_spacing(compact_64_opcodes_,scaled_font_tuples_,request.face_name,target.font_size);
    FixedKinsokuLayoutParameters fixed{parameters,static_cast<int32_t>(target.font_size),
        static_cast<int32_t>(target.font_size),ruby!=0 ? static_cast<int32_t>(text_metrics_[4]) : 0,
        kinsoku!=0,true,text_metrics_[5]!=0,style_x};
    *provider=with_character_bitmaps(std::move(*provider),operands[2],target.font_size,target.font_scale,
        compact_64_opcodes_?uint32_t(style_x)*(style==2?2U:1U):0U,compact_64_opcodes_?uint32_t(style_y)*(style==2?2U:1U):0U);
    std::vector<GlyphMask> full_glyphs;
    auto layout = !compact_64_opcodes_
        ? layout_legacy_text(decoded,fixed,request.font,*provider,proportional,graphics_reciprocal_,
            style==2?uint32_t(style_x)*2U:uint32_t(style_x),style==2?&full_glyphs:nullptr,ruby!=0,aliases)
        : proportional
        ? layout_pe64_proportional_text(decoded,fixed,request.font,*provider,graphics_reciprocal_,
            style==2?uint32_t(style_x)*2U:uint32_t(style_x),style==2?&full_glyphs:nullptr,ruby!=0,aliases)
        : compact_64_opcodes_
        ? layout_pe64_fixed_text(decoded,fixed,request.font,*provider,ruby!=0,aliases,style==2?&full_glyphs:nullptr)
        : layout_plain_fixed_kinsoku_text(decoded,fixed,request.font,*provider);
    if (!layout.layout) {
        if (debug_trace(DebugTrace::TextTiming)) {
            std::cout << "DIRECT_TEXT_LAYOUT_FAILURE error=" << static_cast<unsigned>(layout.error)
                      << " font_size=" << target.font_size << " scale=" << target.font_scale
                      << " ruby=" << ruby << " kinsoku=" << kinsoku << " style=" << style
                      << " codepoints=" << std::hex;
            for (char16_t c : decoded) std::cout << static_cast<uint32_t>(c) << ',';
            std::cout << std::dec << '\n';
        }
        std::ostringstream detail;
        detail<<"layout_error="<<(layout.error==PlainTextLayoutError::UnsupportedControlOrTag?
            "unsupported_control_or_tag":layout.error==PlainTextLayoutError::MissingGlyph?
            "missing_glyph":layout.error==PlainTextLayoutError::InvalidState?"invalid_state":"none")
            <<" profile="<<(compact_64_opcodes_?"PE64":"PE32")
            <<" font="<<target.font_name<<" height="<<target.font_size<<" scale="<<target.font_scale
            <<" proportional="<<proportional<<" ruby="<<ruby<<" kinsoku="<<kinsoku
            <<" cursor="<<parameters.cursor_x<<','<<parameters.cursor_y
            <<" bounds="<<parameters.bounds_left<<','<<parameters.bounds_right
            <<" tuple="<<std::hex;
        for(auto value:operands)detail<<value<<',';
        detail<<" text_utf16=";
        for(std::size_t i=0;i<std::min<std::size_t>(decoded.size(),128);++i)
            detail<<static_cast<uint32_t>(decoded[i])<<',';
        return failure(detail.str());
    }
    if(setting(0x80000009U)!=0)
        cache_paragraph_line_heights(render_cache_records_,next_rich_text_object_id_,
            layout.layout->lines,static_cast<uint32_t>(parameters.line_advance));
    if(!apply_original_text_style(*layout.layout,style,style_x,style_y,
        style?style_color:0,style?256U-style_opacity:0,full_glyphs,target.font_size*target.font_scale*2U/100U,target.font_size*3U/2U,proportional))
        return failure("direct glyph style kernel unsupported");
    if (!apply_original_text_ruby(*layout.layout,request,family==4U?operands[4]:operands[2],
        style,style_percent_x,style_percent_y,style_color,style?256U-style_opacity:0))
        return failure("direct auxiliary ruby layout unsupported");

    // 0043E440 copies each completed node to +0x194 using mode 0. Compose into
    // temporary storage so unavailable bitmap kernels cannot partially commit.
    uint32_t destination = 0xFFFFFFFFU;
    while (graphics_surfaces_.count(destination) != 0 || graphics_surfaces_.count(destination - 1U) != 0) --destination;
    const uint32_t source = destination - 1U;
    GraphicsSurface plane;
    plane.width = target.width;
    plane.height = target.height;
    plane.format = target.plane_format;
    plane.bytes_per_pixel = bitmap_bytes_per_pixel(plane.format);
    plane.stride = plane.width * plane.bytes_per_pixel;
    plane.pixels = target.overlay_plane;
    plane.pixels.resize(static_cast<std::size_t>(plane.stride) * plane.height, 0);
    const auto previous_blits = bitmap_blits_.size();
    const bool previous_dirty = display_dirty_;
    const auto cleanup = [&] {
        graphics_surfaces_.erase(destination);
        graphics_surfaces_.erase(source);
        bitmap_blits_.resize(previous_blits);
        display_dirty_ = previous_dirty;
    };
    graphics_surfaces_[destination] = std::move(plane);
    try {
        for (const auto& node : layout.layout->nodes) {
            GraphicsSurface glyph;
            glyph.width = node.width;
            glyph.height = node.height;
            glyph.format = 2;
            glyph.bytes_per_pixel = 4;
            glyph.stride = node.width * 4U;
            glyph.pixels = node.bitmap;
            graphics_surfaces_[source] = std::move(glyph);
            if (!record_bitmap_blit(destination, source, node.x, node.y, 0, 0)) {
                cleanup();
                return failure("target glyph bitmap copy unsupported");
            }
        }
        object->second.overlay_plane = std::move(graphics_surfaces_.at(destination).pixels);
        object->second.field_18c = 1;
        object->second.overlay_level = 0;
        for (auto& layer : object->second.image_layers) layer.enabled = 0;
        object->second.text_cursor_x_390 = layout.layout->cursor_x;
        object->second.text_cursor_y_394 = layout.layout->cursor_y;
        cleanup();
    } catch (...) {
        cleanup();
        throw;
    }
    state.plane_executed = true;
    if (debug_enabled()) direct_text_draw_states_.push_back(std::move(state));
    display_dirty_ = true;
    return 0;
}
} // namespace bgiplayer

namespace bgiplayer {
// Original 0040BFD0 -> 0040C090 (2->1), 0040C210 (2->2).
bool VM::record_bitmap_mode0_alpha(uint32_t destination, uint32_t source,
                                   int32_t x, int32_t y, uint32_t mode, uint32_t parameter) {
    auto target = graphics_surfaces_.find(destination);
    auto input = graphics_surfaces_.find(source);
    if (target == graphics_surfaces_.end() || input == graphics_surfaces_.end() ||
        !materialize_graphics_pixels(target->second) || !materialize_graphics_pixels(input->second)) return false;
    auto& output = target->second;
    auto& image = input->second;
    const int64_t left = std::max<int64_t>(x, 0);
    const int64_t top = std::max<int64_t>(y, 0);
    const int64_t right = std::min<int64_t>(static_cast<int64_t>(x) + image.width - 1, static_cast<int64_t>(output.width) - 1);
    const int64_t bottom = std::min<int64_t>(static_cast<int64_t>(y) + image.height - 1, static_cast<int64_t>(output.height) - 1);
    // Original C090/C210 execute adjacent pairs, followed by a distinct odd
    // tail. Preserve the clipped row width: splitting a row changes dispatch.
    // C210's pair uses RCPPS then truncation; its odd tail uses integer DIV.
    // The 8-bit alpha domain is finite. Sparse corrections retain all 65,536
    // measured original pair weights without relying on host reciprocal ISA.
    static const auto reciprocal_corrections = [] {
        std::array<uint8_t, 65536> result{};
        constexpr uint32_t entries[] = {
#include "original_mode0_alpha_corrections.inc"
        };
        for (const uint32_t entry : entries)
            result[entry >> 3U] = static_cast<uint8_t>(entry & 7U);
        return result;
    }();
    if(left<=right && top<=bottom) {
        const auto width=static_cast<uint32_t>(right-left+1),height=static_cast<uint32_t>(bottom-top+1);
        const auto d0=static_cast<uint64_t>(top)*output.stride+static_cast<uint64_t>(left)*4U;
        const auto s0=static_cast<uint64_t>(top-y)*image.stride+static_cast<uint64_t>(left-x)*4U;
        if(d0<=UINT32_MAX && s0<=UINT32_MAX && compose_host_layer_composite({
            {output.pixels,static_cast<uint32_t>(d0),output.stride,width,height,output.format,4},
            {image.pixels,static_cast<uint32_t>(s0),image.stride,width,height,2,4},
            reciprocal_corrections}).success) {
            refresh_bgra_mirror(output);
            if(debug_enabled())bitmap_blits_.push_back({destination,source,x,y,mode,parameter});
            display_dirty_=true;return true;
        }
    }
    for (int64_t row = top; row <= bottom; ++row) {
        for (int64_t column = left; column <= right; column += 2) {
            const uint32_t count = column < right ? 2U : 1U;
            const auto source_offset = static_cast<std::size_t>(row - y) * image.stride +
                                       static_cast<std::size_t>(column - x) * 4U;
            const auto output_offset = static_cast<std::size_t>(row) * output.stride +
                                       static_cast<std::size_t>(column) * 4U;
            const uint32_t first_alpha = image.pixels[source_offset + 3U];
            const uint32_t second_alpha = count == 2U ? image.pixels[source_offset + 7U] : 0U;
            if (count == 2U && first_alpha == 0U && second_alpha == 0U) continue;
            const bool opaque_pair = count == 2U &&
                (output.format == 1U ? first_alpha >= 254U && second_alpha >= 254U
                                     : first_alpha == 255U && second_alpha == 255U);
            if (opaque_pair) {
                std::copy_n(image.pixels.begin() + static_cast<std::ptrdiff_t>(source_offset), 8,
                            output.pixels.begin() + static_cast<std::ptrdiff_t>(output_offset));
                continue;
            }
            for (uint32_t pixel = 0; pixel < count; ++pixel) {
                const std::size_t src = source_offset + pixel * 4U;
                const std::size_t dst = output_offset + pixel * 4U;
                const uint32_t alpha = image.pixels[src + 3U];
                if (output.format == 1U) {
                    uint32_t factor = alpha >> 1U;
                    // 00408653..0040868A overwrites table entry127 with128.
                    if (factor == 127U) {
                        std::copy_n(image.pixels.begin() + static_cast<std::ptrdiff_t>(src), 3,
                                    output.pixels.begin() + static_cast<std::ptrdiff_t>(dst));
                        if (count == 1U) output.pixels[dst + 3U] = 0;
                    } else if (factor != 0U) {
                        // 00530650's fourth word is zero: alpha is retained.
                        for (uint32_t byte = 0; byte < 3; ++byte) {
                            const int32_t old = output.pixels[dst + byte];
                            const int32_t delta = static_cast<int32_t>(image.pixels[src + byte]) - old;
                            output.pixels[dst + byte] = static_cast<uint8_t>(
                                old + ((delta * static_cast<int32_t>(factor)) >> 7));
                        }
                    }
                } else {
                    if (count == 1U && alpha == 0U) continue;
                    if (count == 1U && alpha == 255U) {
                        std::copy_n(image.pixels.begin() + static_cast<std::ptrdiff_t>(src), 4,
                                    output.pixels.begin() + static_cast<std::ptrdiff_t>(dst));
                        continue;
                    }
                    const uint32_t destination_alpha = output.pixels[dst + 3U];
                    const uint32_t destination_component = destination_alpha * (256U - alpha);
                    const uint32_t total = alpha * 256U + destination_component;
                    int32_t source_weight = total == 0 ? 0 : (alpha << 16U) / total;
                    int32_t destination_weight = total == 0 ? 0 : (destination_component << 8U) / total;
                    if (count == 2U) {
                        switch (reciprocal_corrections[alpha * 256U + destination_alpha]) {
                            case 1: --source_weight; break;
                            case 2: ++source_weight; break;
                            case 3: --destination_weight; break;
                            case 4: ++destination_weight; break;
                            default: break;
                        }
                    }
                    for (uint32_t byte = 0; byte < 3; ++byte) {
                        const uint32_t value = image.pixels[src + byte] * source_weight +
                                               output.pixels[dst + byte] * destination_weight;
                        output.pixels[dst + byte] = static_cast<uint8_t>((value & 0xFFFFU) >> 8U);
                    }
                    output.pixels[dst + 3U] = static_cast<uint8_t>(total >> 8U);
                }
            }
        }
    }
    refresh_bgra_mirror(output);
    if (debug_enabled()) bitmap_blits_.push_back({destination, source, x, y, mode, parameter});
    display_dirty_ = true;
    return true;
}
} // namespace bgiplayer

namespace bgiplayer {
uint32_t VM::configure_rich_text_procedure(uint8_t family,uint8_t subopcode,
    std::span<const uint32_t> operands,std::string text) {
    if(operands.size()<2)return 1;
    auto object=window_objects_.find(operands[0]);
    if(object==window_objects_.end())return 0xFFFFFFFFU;
    RichTextProcedureState state;
    state.family=family;state.subopcode=subopcode;state.window=operands[0];
    state.wrapper_operands.assign(operands.begin(),operands.end());state.text=std::move(text);
    auto fail=[&](const char* reason){state.unsupported_reason=reason;
        rich_text_procedure_states_.push_back(std::move(state));return 1U;};
    const bool extended=family==4;
    const bool styled=family==3 && subopcode==0x92;
    if((family!=3 && family!=4) || (family==4 && subopcode!=0x90) ||
       (family==3 && subopcode!=0x90 && subopcode!=0x92) ||
       operands.size()!=(extended?15U:styled?11U:10U))return fail("unsupported rich-text ABI");
    if(!window_glyph_provider_factory_)return fail("window glyph provider unavailable");
    auto& target=object->second;
    if(target.font_name.empty() || target.font_size==0 || target.font_scale==0)
        return fail("window font unavailable");
    if(target.text_boundary_mode_398!=0 || target.text_field_39c!=0)
        return fail("vertical or aligned rich text unsupported");
    const bool proportional=target.font_field5!=0;
    if(proportional) {
        const auto tuple=scaled_font_tuples_.find(target.font_name);
        if(!compact_64_opcodes_ && tuple!=scaled_font_tuples_.end() && tuple->second[4]!=0)
            return fail("proportional rich transformed font spacing unsupported");
    }
    const uint32_t ruby=operands[extended?3U:styled?8U:7U];
    const uint32_t kinsoku=operands[extended?5U:styled?9U:8U];
    const uint32_t style=extended?operands[6]:grp0_render_global_529bbc_;
    const uint32_t wait_after=operands[extended?12U:styled?5U:4U];
    if(style>2 || (styled && operands[3]!=0))
        return fail("rich style or styled-frontend override unsupported");
    state.post_display_wait=wait_after;
    state.force_completion=operands[extended?11U:styled?4U:3U];
    state.enable_high_input=operands[extended?13U:styled?6U:5U];
    state.enable_mask_input=operands[extended?14U:styled?7U:6U];
    if(grp0_render_global_532828_==2)return fail("window-specific rich input priority unsupported");
    state.input_key=grp0_render_global_532828_==1?(grp0_render_global_53281c_<<16U)|0xFFFFU:2U;
    WindowGlyphRequest request;
    request.window=state.window;request.face_name=target.font_name;
    request.logical_height=target.font_size;request.width_percent=target.font_scale;
    request.bold=target.font_field4;request.proportional=target.font_field5;request.font_field6=target.font_field6;
    request.font.face=target_utf16(target.font_name,detect_text_encoding(target.font_name));
    request.font.height=static_cast<int32_t>(target.font_size);
    request.font.weight=target.font_field4==0?100:700;
    request.font.quality=2;request.font.outline_gray_bits=6;
    auto provider=window_glyph_provider_factory_(request);
    if(!provider)return fail("explicit window font could not be opened");
    std::u16string decoded;
    try {decoded=target_utf16(state.text,text_encoding_mode_==1?TextEncoding::Utf8:detect_text_encoding(state.text));}
    catch(const std::exception&){return fail("invalid target text encoding");}
    const auto aliases=ruby!=0?window_ruby_dictionary(rich_text_aliases_):std::vector<TextRubyAlias>{};
    bool grouping=true;
    if(!decoded.empty() && decoded.front()==0x0B){grouping=false;decoded.erase(decoded.begin());}
    int32_t font_spacing=0;
    const auto registered=std::find_if(font_cache_registrations_.begin(),font_cache_registrations_.end(),
        [&](const auto& entry){return entry.face_name==target.font_name && entry.parameter_2==target.font_size &&
          entry.parameter_3==target.font_scale && entry.parameter_4==target.font_field4;});
    if(registered!=font_cache_registrations_.end())font_spacing=text_integer_font_spacing(0,
        target.font_field4,registered->first_init,registered->second_init,0);
    const uint32_t line_advance=target.font_size+target.font_size*target.line_spacing_percent/100U;
    const auto setting=[&](uint32_t key,uint32_t initial){const auto found=graphics_settings_.find(key);
        return found==graphics_settings_.end()?initial:found->second;};
    PlainTextLayoutParameters parameters{target.text_cursor_x_390,target.text_cursor_y_394,
        target.update_left,target.update_right,static_cast<int32_t>(target.font_size*target.font_scale/100U),
        static_cast<int32_t>(text_metrics_[2]+static_cast<uint32_t>(font_spacing)),
        static_cast<int32_t>(line_advance),0,text_metrics_[0],text_metrics_[1]==0?1U:text_metrics_[1],
        operands[2],setting(0x80000004U,0)!=0,grouping};
    const int32_t style_x=style==0?0:std::max(1,static_cast<int32_t>(target.font_size*(extended?operands[7]:grp0_render_global_529bc0_)/100U));
    const int32_t style_y=style==0?0:std::max(1,static_cast<int32_t>(target.font_size*(extended?operands[8]:grp0_render_global_529bc4_)/100U));
    if(ruby!=0){
        parameters.baseline_offset=std::max(4,static_cast<int32_t>(grp1_text_style_.field_5328bc)>0?
            static_cast<int32_t>(grp1_text_style_.field_5328bc):static_cast<int32_t>(target.font_size*text_metrics_[3]/100U));
        if(setting(0x80000002U,0)!=0)parameters.baseline_offset-=static_cast<int32_t>(grp1_text_style_.field_53283c);
    }
    state.icon_baseline=ruby!=0?std::max(4,static_cast<int32_t>(grp1_text_style_.field_5328bc)>0?
        static_cast<int32_t>(grp1_text_style_.field_5328bc):static_cast<int32_t>(target.font_size*text_metrics_[3]/100U)):0;
    if(style==2 && setting(0x80000002U,0)!=0)parameters.baseline_offset+=style_y;
    parameters.font_spacing=text_font_spacing(compact_64_opcodes_,scaled_font_tuples_,request.face_name,target.font_size);
    FixedKinsokuLayoutParameters fixed{parameters,static_cast<int32_t>(target.font_size),
        static_cast<int32_t>(target.font_size) /* PE529BE8=1 */,ruby!=0?static_cast<int32_t>(text_metrics_[4]):0,
        kinsoku!=0,true /* PE529BF4=1 */,text_metrics_[5]!=0,style_x};
    // PE64 E50C0→F4300 constructs the 79230 class. Its vtable+68
    // is77F90, which enters75570, also used by direct and bitmap text.
    *provider=with_character_bitmaps(std::move(*provider),operands[2],target.font_size,target.font_scale,
        compact_64_opcodes_?uint32_t(style_x)*(style==2?2U:1U):0U,compact_64_opcodes_?uint32_t(style_y)*(style==2?2U:1U):0U);
    std::vector<GlyphMask> full_glyphs;
    auto layout=!compact_64_opcodes_?layout_legacy_text(decoded,fixed,request.font,*provider,
        proportional,graphics_reciprocal_,style==2?uint32_t(style_x)*2U:uint32_t(style_x),
        style==2?&full_glyphs:nullptr,ruby!=0,aliases)
        :proportional?layout_pe64_proportional_text(decoded,fixed,request.font,*provider,
        graphics_reciprocal_,style==2?uint32_t(style_x)*2U:uint32_t(style_x),style==2?&full_glyphs:nullptr,ruby!=0,aliases)
        :compact_64_opcodes_?layout_pe64_fixed_text(decoded,fixed,request.font,*provider,ruby!=0,aliases,style==2?&full_glyphs:nullptr)
        :layout_plain_fixed_kinsoku_text(decoded,fixed,request.font,*provider);
    if(!layout.layout)return fail("unsupported rich layout branch or missing glyph");
    if(setting(0x80000009U,0)!=0)
        cache_paragraph_line_heights(render_cache_records_,next_rich_text_object_id_,
            layout.layout->lines,line_advance);
    if(!apply_original_text_style(*layout.layout,style,style_x,style_y,
        extended?operands[9]:grp0_render_global_529bcc_,
        0x100U-(extended?operands[10]:grp0_render_global_529bc8_),full_glyphs,target.font_size*target.font_scale*2U/100U,target.font_size*3U/2U,proportional))
        return fail("rich glyph style kernel unsupported");
    if (!apply_original_text_ruby(*layout.layout,request,extended?operands[4]:operands[2],style,
        extended?operands[7]:grp0_render_global_529bc0_,
        extended?operands[8]:grp0_render_global_529bc4_,
        extended?operands[9]:grp0_render_global_529bcc_,
        0x100U-(extended?operands[10]:grp0_render_global_529bc8_)))
        return fail("rich auxiliary ruby layout unsupported");
    state.layout=std::move(*layout.layout);
    for(const auto& node:state.layout.nodes)
        state.nodes.push_back({0,node.start_delay,node.elapsed,node.fade_duration,node.x,node.y,node.kind});
    state.last_clock_ms=monotonic_ms();state.fixed_clock=state.last_clock_ms<<16U;
    state.clock_origin=static_cast<uint32_t>(state.last_clock_ms);
    state.wait_due_ms=state.clock_origin;
    state.plane_executed=true;
    if(setting(0x80000006U,0)!=0)target.text_saved_plane=target.overlay_plane;
    target.field_18c=1;target.overlay_level=0;
    for(auto& layer:target.image_layers)layer.enabled=0;
    target.text_cursor_x_390=state.layout.cursor_x;target.text_cursor_y_394=state.layout.cursor_y;
    rich_text_procedure_states_.push_back(std::move(state));
    const std::size_t index=rich_text_procedure_states_.size()-1U;
    arm_input_capture(rich_text_procedure_states_[index].input_key);
    (void)poll_input_capture(rich_text_procedure_states_[index].input_key);
    thread_.snapshot_wait_kind = SnapshotWaitKind::RichText;
    thread_.snapshot_wait_index = index;
    thread_.triple_command_queue.clear();
    thread_.wait_procedure=[this,index](){return advance_rich_text_procedure(index);};
    thread_.status_flags|=1U;thread_.state=ThreadState::Waiting;
    display_dirty_=true;
    return 0;
}

// Grp0:90 creates the base CProcDspMsg, whose 0C dispatch is 438C60.
// Reuse the serializable procedure clock/counter fields; family 2 identifies
// this base procedure independently from rich glyph nodes.
uint32_t VM::configure_text_display_procedure(
    const std::array<uint32_t,6>& operands, std::string text) {
    const auto object=window_objects_.find(operands[0]);
    if(object==window_objects_.end())return 0xFFFFFFFFU;
    if(object->second.width==0 || object->second.height==0)return 1;
    TextProcedureState retained;
    retained.wrapper_operands=operands;
    retained.core_arguments={static_cast<int32_t>(operands[0]),0,
        static_cast<int32_t>(operands[2]),0,static_cast<int32_t>(operands[2]),0,0,0,
        static_cast<int32_t>(operands[3]),static_cast<int32_t>(operands[4]),
        static_cast<int32_t>(operands[5]),1,-1};
    retained.text=text;retained.window=operands[0];
    text_procedure_states_.push_back(std::move(retained));
    // The actual scrmsg clear-page string is a single form feed. Other
    // base-procedure bytecodes need their own 438A80 interpreter branches.
    if(text!=std::string(1,'\f'))return 1;
    RichTextProcedureState state;
    state.family=2;state.subopcode=0x90;state.window=operands[0];
    state.wrapper_operands.assign(operands.begin(),operands.end());
    state.text=std::move(text);state.plane_executed=true;
    state.force_completion=operands[3];state.post_display_wait=operands[4];
    state.enable_high_input=operands[5];state.enable_mask_input=1;
    if(state.post_display_wait!=0)return 1;
    if(grp0_render_global_532828_==2)return 1;
    state.input_key=grp0_render_global_532828_==1?(grp0_render_global_53281c_<<16U)|0xFFFFU:2U;
    const uint64_t now=monotonic_ms();state.last_clock_ms=now;
    state.wait_due_ms=static_cast<uint32_t>(now);
    rich_text_procedure_states_.push_back(std::move(state));
    auto& target=object->second;
    target.overlay_level=0;target.field_18c=1;
    for(auto& layer:target.image_layers)layer.enabled=0;
    const std::size_t index=rich_text_procedure_states_.size()-1U;
    arm_input_capture(rich_text_procedure_states_[index].input_key);
    (void)poll_input_capture(rich_text_procedure_states_[index].input_key);
    thread_.snapshot_wait_kind=SnapshotWaitKind::RichText;
    thread_.snapshot_wait_index=index;
    thread_.triple_command_queue.clear();
    thread_.wait_procedure=[this,index](){return advance_rich_text_procedure(index);};
    thread_.status_flags|=1U;thread_.state=ThreadState::Waiting;
    display_dirty_=true;
    return 0;
}

WaitProcedureResult VM::advance_rich_text_procedure(std::size_t index) {
    auto& thread = current_execution_thread();
    bool cancel = false;
    // Original070410/436E50 drains the CURRENT wait object's FIFO before
    // polling text. Message0 sets its base cancel flag and stops the drain.
    while (!thread.triple_command_queue.empty()) {
        const auto message = thread.triple_command_queue.front();
        thread.triple_command_queue.pop_front();
        if (message[0] == 0) {
            cancel = true;
            break;
        }
        auto& state = rich_text_procedure_states_.at(index);
        // CProcDspMsg virtual message handler: PE32 00439610 / PE64
        // 1400717E0. These mutate the current procedure before its ordinary
        // tick; a skip is neither cancellation nor a synthetic mouse event.
        switch (message[0]) {
            case 1U:
            case 0x102U:
                if (state.family != 2) state.auto_finished = true;
                state.force_completion = 1;
                break;
            case 0x100U:
                if (state.family != 2) state.auto_finished = true;
                break;
            case 0x101U:
                state.enable_high_input = message[1];
                break;
            case 3U:
                // PE64 intercepts this before the virtual handler and sets
                // its independent base-liveness override (+1C).
                if (compact_64_opcodes_)
                    throw BpFault("rich text base-liveness message is unsupported: 3");
                break;
            default:
                // The original virtual handler ignores other message IDs.
                break;
        }
    }
    auto result = advance_rich_text_procedure_body(index);
    if (cancel) {
        // 072670 still calls the ordinary text tick before checking070600.
        // Cancellation then completes with the existing input-force word;
        // it does not fast-reveal glyphs or synthesize a pointer click.
        auto& state = rich_text_procedure_states_.at(index);
        if (!state.complete) {
            state.complete = true;
            disarm_input_capture(state.input_key);
        }
        thread.triple_command_queue.clear();
        return {1, {state.input_force}};
    }
    return result;
}

WaitProcedureResult VM::advance_rich_text_procedure_body(std::size_t index) {
    auto& state=rich_text_procedure_states_.at(index);
    if(state.complete)return {1,{state.input_force}};
    const bool trace_text_timing=debug_trace(DebugTrace::TextTiming);
    const uint32_t pending_left=trace_text_timing?input_descriptor_event_values_[1]:0;
    uint32_t input=poll_input_capture(state.input_key)&(sys1_input_global_529bac_|0x80000181U);
    if(state.enable_high_input==0)input&=0x7FFFFFFFU;
    if(state.enable_mask_input==0)input&=~(sys1_input_global_529bac_|0x80U);
    if(trace_text_timing && input==0 && (pending_left!=0 || (input_descriptor(1)&0x8000U)!=0)) {
        std::cout<<"RICH_TEXT_BLOCKED mono="<<monotonic_ms()<<" index="<<index<<" key="<<state.input_key
                 <<" pending="<<pending_left<<" gate="<<input_capture_gate(state.input_key)
                 <<" hit="<<input_region_hit(state.input_key)<<" regions=";
        const auto pointer=pointer_position();
        for(const auto& region:input_regions_)if(region.left<=pointer.first && pointer.first<=region.right &&
            region.top<=pointer.second && pointer.second<=region.bottom)
            std::cout<<region.priority<<':'<<region.object<<',';
        std::cout<<'\n';
    }
    if(input!=0){state.input_force=1;if(state.family!=2 && graphics_flag_532814_!=0)state.auto_finished=true;}
    if(input!=0 && trace_text_timing)
        std::cout<<"RICH_TEXT_INPUT mono="<<monotonic_ms()<<" index="<<index<<" input="<<input
                 <<" drawn="<<text_procedure_nodes_complete(state.nodes)<<" entered="<<state.click_wait_entered
                 <<" forced="<<state.force_completion<<" post="<<state.post_display_wait<<'\n';
    if(state.family==2 && state.subopcode==0x90){
        auto found=window_objects_.find(state.window);
        if(found==window_objects_.end())throw BpFault("base text window released during page clear");
        auto& target=found->second;
        uint32_t now=static_cast<uint32_t>(monotonic_ms());
        if(static_cast<int32_t>(now-state.wait_due_ms)<0 && state.input_force==0 && state.force_completion==0)return {0,{}};
        if(!state.click_wait_entered){
            // 438C60 snapshots these globals when +64 is zero.
            state.clock_origin=graphics_count_529bdc_;
            state.auto_deadline_ms=graphics_flag_529bd0_;
            state.click_wait_entered=true;
            if(trace_text_timing)std::cout<<"RICH_TEXT_WAIT mono="<<monotonic_ms()
                <<" index="<<index<<" input="<<input<<" force="<<state.input_force<<'\n';
        }
        const bool forced=state.input_force!=0 || state.force_completion!=0;
        do {
            // 438A80 interprets the terminating NUL only on the next due
            // iteration. auto_finished is otherwise unused by this family.
            if(state.auto_finished){
                state.complete=true;
                disarm_input_capture(state.input_key);
                return {1,{state.input_force}};
            }
            // 438B40/436D90 accumulate deadlines, so a slow host frame
            // catches up every elapsed fade stage instead of stretching it.
            if(state.auto_deadline_ms!=0 && !forced)
                state.wait_due_ms+=state.auto_deadline_ms;
            else state.wait_due_ms=now;
            if(static_cast<int32_t>(state.clock_origin-1U)<=static_cast<int32_t>(state.icon_frame)){
                // 4308C0 clears only +194; layers/records and +18C survive.
                std::fill(target.overlay_plane.begin(),target.overlay_plane.end(),0);
                target.overlay_level=0;
                (void)set_window_text_direction(state.window,target.text_boundary_mode_398);
                state.icon_frame=0;
                state.auto_finished=true;
                display_dirty_=true;
            }else{
                ++state.icon_frame;
                target.overlay_level=state.icon_frame*256U/state.clock_origin;
                display_dirty_=true;
            }
            now=static_cast<uint32_t>(monotonic_ms());
        }while(forced || static_cast<int32_t>(now-state.wait_due_ms)>=0);
        return {0,{}};
    }
    const auto setting=[&](uint32_t key,uint32_t initial){const auto found=graphics_settings_.find(key);
        return found==graphics_settings_.end()?initial:found->second;};
    const uint64_t now=monotonic_ms();
    // Original43EE50 uses unit speed after all node completion.
    const bool drawn=text_procedure_nodes_complete(state.nodes);
    state.fixed_clock+=(now-state.last_clock_ms)*(drawn?0x10000U:setting(0x80000001U,0x10000U));
    state.last_clock_ms=now;
    const uint32_t clock=static_cast<uint32_t>(state.fixed_clock>>16U);
    // 0043A5D0 tests completion before ticking; final draw returns0 until
    // the following poll enters39230/39320's distinct post-display phase.
    if(drawn){
        auto object=window_objects_.find(state.window);
        if(object==window_objects_.end())throw BpFault("rich text window released during click wait");
        auto& target=object->second;
        const auto finish=[&](){target.image_layers[0].enabled=0;state.complete=true;disarm_input_capture(state.input_key);display_dirty_=true;
            if(trace_text_timing)std::cout<<"RICH_TEXT_FINISH mono="<<monotonic_ms()
                <<" index="<<index<<" input_force="<<state.input_force<<" post="<<state.post_display_wait<<'\n';
            return WaitProcedureResult{1,{state.input_force}};};
        if(state.post_display_wait==0 || state.auto_finished)return finish();
        if(graphics_first_532810_!=0 && !state.click_wait_entered)
            state.auto_deadline_ms=static_cast<uint32_t>(now)+graphics_second_532818_;
        if(static_cast<int32_t>(clock-state.wait_due_ms)<0 && state.input_force==0 && state.force_completion==0)return {0,{}};
        if(!state.click_wait_entered){
            uint32_t width=0;for(const auto& icon:icon_images_)width=std::max(width,icon.image.width);
            if(static_cast<int32_t>(static_cast<uint32_t>(target.text_cursor_x_390)+width)>
                   static_cast<int32_t>(static_cast<uint32_t>(target.update_right)+1U) && grp0_render_53100c_!=2U){
                // 004315A0 resetsX even when the vertical space test fails.
                // 00438D50 then marks+60 and returns0; it does not scroll or
                // discard the already rendered paragraph.
                target.text_cursor_x_390=target.update_left;
                const uint32_t line=target.font_size+target.font_size*target.line_spacing_percent/100U;
                const int32_t required=static_cast<int32_t>(static_cast<uint32_t>(target.text_cursor_y_394)+line*2U);
                if(required>static_cast<int32_t>(static_cast<uint32_t>(target.update_bottom)+1U)){
                    state.pending_line_break=true;
                    return {0,{}};
                }
                target.text_cursor_y_394=static_cast<int32_t>(static_cast<uint32_t>(target.text_cursor_y_394)+line);
                // Virtual+2C is43E4B0 for CProcDspMsgEx:31680(39A10()).
                target.text_cursor_x_390=static_cast<int32_t>(static_cast<uint32_t>(target.text_cursor_x_390)+text_metrics_[4]);
            }
            input&=0x80000000U;
            if(input==0)state.input_force=0;
            state.force_completion=0;
            state.click_wait_entered=true;
            if(trace_text_timing)std::cout<<"RICH_TEXT_WAIT mono="<<monotonic_ms()
                <<" index="<<index<<" input="<<input<<" force="<<state.input_force<<'\n';
        }
        if(input==0){
            if(!icon_images_.empty()){
                const auto& icon=icon_images_[state.icon_frame];
                state.icon_frame=(state.icon_frame+1U)%static_cast<uint32_t>(icon_images_.size());
                if(icon.source==-1 || icon.image.pixels.empty())target.image_layers[0].enabled=0;
                else{
                    auto& layer=target.image_layers[0];layer.image=icon.image;
                    layer.x=static_cast<int32_t>(grp0_render_532820_);
                    layer.y=static_cast<int32_t>(grp0_render_532824_);
                    if(grp0_render_53100c_!=1U){layer.x+=target.text_cursor_x_390;
                        layer.y+=target.text_cursor_y_394+state.icon_baseline;}
                    layer.level=0;layer.enabled=1;
                }
            }
            state.wait_due_ms+=grp0_render_global_529be0_;
            display_dirty_=true;
            if(graphics_first_532810_!=0 && static_cast<uint32_t>(now)>=state.auto_deadline_ms)state.auto_finished=true;
            return {0,{}};
        }
        return finish();
    }
    const bool elapsed_mode=setting(0x80000000U,0)!=0;
    const bool redraw=setting(0x80000006U,0)!=0;
    if(elapsed_mode && state.first_elapsed_poll){state.clock_origin=clock;state.first_elapsed_poll=false;}
    const uint32_t steps=static_cast<int32_t>(clock-state.clock_origin)<0?0U:clock-state.clock_origin+1U;
    const auto text_draw_began=trace_text_timing?raw_monotonic_ms():0;
    const auto actions=tick_text_procedure(state.nodes,{state.force_completion!=0 || state.input_force!=0 || text_metrics_[0]==0,
        elapsed_mode,redraw,false,steps,clock-state.clock_origin,text_metrics_[1]==0?1U:text_metrics_[1]});
    const auto object=window_objects_.find(state.window);
    if(object==window_objects_.end())throw BpFault("rich text window released during procedure");
    auto& target=object->second;
    uint32_t destination=0xFFFFFFFFU;
    while(graphics_surfaces_.count(destination)!=0 || graphics_surfaces_.count(destination-1U)!=0)--destination;
    const uint32_t source=destination-1U;
    GraphicsSurface plane;
    plane.width=target.width;plane.height=target.height;plane.format=target.plane_format;
    plane.bytes_per_pixel=bitmap_bytes_per_pixel(plane.format);plane.stride=plane.width*plane.bytes_per_pixel;
    plane.pixels=target.overlay_plane;plane.pixels.resize(static_cast<std::size_t>(plane.stride)*plane.height,0);
    graphics_surfaces_[destination]=std::move(plane);
    const auto blits=bitmap_blits_.size();
    try {
        for(const auto& action:actions) {
            if(action.kind==TextProcedureActionKind::ResetClock){state.clock_origin=clock;state.wait_due_ms=clock;continue;}
            if(action.kind==TextProcedureActionKind::ClearWholePlane){
                auto& pixels=graphics_surfaces_.at(destination).pixels;
                if(target.text_saved_plane.size()==pixels.size())pixels=target.text_saved_plane;continue;}
            if(action.kind!=TextProcedureActionKind::ClearNode && action.kind!=TextProcedureActionKind::BlendNode)continue;
            const auto& node=state.layout.nodes.at(action.node);
            GraphicsSurface glyph;
            glyph.width=node.width;glyph.height=node.height;glyph.format=2;glyph.bytes_per_pixel=4;
            glyph.stride=node.width*4U;glyph.pixels=node.bitmap;graphics_surfaces_[source]=std::move(glyph);
            if(!record_bitmap_blit(destination,source,node.x,node.y,
                action.kind==TextProcedureActionKind::ClearNode?0x40U:1U,action.parameter))
                throw BpFault("rich text node bitmap operation unsupported");
        }
        target.overlay_plane=std::move(graphics_surfaces_.at(destination).pixels);
    } catch(...) {
        graphics_surfaces_.erase(destination);graphics_surfaces_.erase(source);bitmap_blits_.resize(blits);throw;
    }
    graphics_surfaces_.erase(destination);graphics_surfaces_.erase(source);bitmap_blits_.resize(blits);
    display_dirty_=true;
    if(trace_text_timing)std::cout<<"RICH_TEXT_DRAW mono="<<monotonic_ms()<<" index="<<index<<" cost_ms="<<(raw_monotonic_ms()-text_draw_began)<<" actions="<<actions.size()<<" complete="<<text_procedure_nodes_complete(state.nodes)<<" force="<<state.input_force<<'\n';
    return {0,{}};
}
} // namespace bgiplayer
