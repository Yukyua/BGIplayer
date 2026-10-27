#include "bgiplayer/vm.hpp"
#include "bgiplayer/background_transform.hpp"
#include "bgiplayer/sprite_reduced_cache.hpp"
namespace bgiplayer {
GlyphOutlineProvider VM::with_character_bitmaps(GlyphOutlineProvider host,uint32_t color,
    uint32_t logical_height,uint32_t width_percent,uint32_t padding_x,uint32_t padding_y) {
    return [this,host=std::move(host),color,logical_height,width_percent,padding_x,padding_y](const GlyphFontRequest& font,uint32_t codepoint)->std::optional<GlyphMask> {
        // Unicode parser 77036 and measurement 797E2 only add the sign tag.
        // Byte conversion/crop use FA3B0 separately; U+FF01 is ordinary Unicode.
        if(codepoint<0xF001U || codepoint>0xF7FFU)return host?host(font,codepoint):std::nullopt;
        GlyphMask glyph;glyph.character_bitmap=true;
        // PE32 438400/438380 use a low-byte descriptor array; PE64 72F20
        // uses its complete-key map. Do not extend either profile's rule.
        const uint32_t key=compact_64_opcodes_ ? codepoint|0x80000000U
            : 0x8000F000U|(codepoint&0xFFU);
        const auto source=graphics_surfaces_.find(key);
        if(source==graphics_surfaces_.end())return glyph; // original absent slot: no node/advance/delay.
        auto quant=graphics_settings_.find(0x8000000AU);
        // PE32 438400 has no corresponding setting branch: its slot copy
        // remains unscaled. 8000000A selects this arm only in PE64 72F20.
        const bool scaled=compact_64_opcodes_ && quant!=graphics_settings_.end() && quant->second!=0;
        auto& input=source->second;
        if(input.format>3 || !materialize_graphics_pixels(input))
            throw BpFault("character-bitmap glyph source requires a supported color canvas");
        glyph.width=input.width;glyph.height=input.height;glyph.stride=input.width;
        glyph.character_bitmap_source_width=input.width;glyph.character_bitmap_scaled=scaled;
        glyph.source_format=input.format;
        glyph.metrics.words[5]=input.width?input.width-1U:0U;
        glyph.metrics.words[6]=input.height?input.height-1U:0U;
        glyph.alpha.resize(size_t(input.width)*input.height);
        for(uint32_t y=0;y<input.height;++y)for(uint32_t x=0;x<input.width;++x)
            glyph.alpha[size_t(y)*input.width+x]=input.format==3?input.pixels[size_t(y)*input.stride+x]:
                input.format==2?input.pixels[size_t(y)*input.stride+x*4U+3U]:255U;
        if(input.format==0) {
            // Unscaled 72F20 -> 3E220 (PE32 438400 -> 40BFD0) is a
            // byte-count copy, even across this 16-to-32-bit descriptor pair.
            // Its two-byte row is not a request for RGB565 conversion.
            glyph.bitmap.assign(size_t(input.width)*input.height*4U,0);
            for(uint32_t y=0;y<input.height;++y)std::copy_n(input.pixels.data()+size_t(y)*input.stride,
                size_t(input.width)*2U,glyph.bitmap.data()+size_t(y)*input.width*4U);
            for(size_t i=0;i<glyph.alpha.size();++i)glyph.alpha[i]=glyph.bitmap[i*4U+3U];
        } else {
        uint32_t scratch=0xFFFFFFF0U;
        while(graphics_surfaces_.contains(scratch)||graphics_surfaces_.contains(scratch-1U))scratch-=2U;
        GraphicsSurface destination;destination.width=input.width;destination.height=input.height;
        destination.format=2;destination.bytes_per_pixel=4;destination.stride=input.width*4U;
        destination.pixels.resize(size_t(destination.stride)*destination.height);
        const bool old_dirty=display_dirty_;const size_t history=bitmap_blits_.size();
        const auto cleanup=[&]{graphics_surfaces_.erase(scratch);graphics_surfaces_.erase(scratch-1U);
            bitmap_blits_.resize(history);display_dirty_=old_dirty;};
        try {
            graphics_surfaces_[scratch]=destination;uint32_t blit_source=source->first;
            if(input.format==3) {
                // 72F20 -> 45E10 uses the alpha-copy kernel, not font-mask colorization.
                auto colored=destination;
                for(size_t i=0;i<glyph.alpha.size();++i){colored.pixels[i*4U]=uint8_t(color);
                    colored.pixels[i*4U+1U]=uint8_t(color>>8);colored.pixels[i*4U+2U]=uint8_t(color>>16);
                    colored.pixels[i*4U+3U]=glyph.alpha[i];}
                graphics_surfaces_[scratch-1U]=std::move(colored);blit_source=scratch-1U;
            }
            if(!record_bitmap_blit(scratch,blit_source,0,0,0,0))
                throw BpFault("character-bitmap glyph original copy failed");
            glyph.bitmap=std::move(graphics_surfaces_.at(scratch).pixels);cleanup();
        }catch(...){cleanup();throw;}
        }
        if(scaled) {
            // 72F20 samples using font height/width-percent. Parser7707B uses
            // a height-square canvas plus style padding independently, while
            // measurement 796C0 keeps the registered slot width.
            const uint32_t height=logical_height?logical_height:uint32_t(font.height);
            const uint32_t width=height+padding_x,canvas_height=height+padding_y;
            if(height>INT32_MAX || width<height || canvas_height<height || width>INT32_MAX || canvas_height>INT32_MAX || uint64_t(height)*width_percent>INT32_MAX-50U ||
               input.width==0 || input.height==0 || input.width>UINT32_MAX/100U ||
               uint64_t(width)*canvas_height>SIZE_MAX/4U || width>UINT32_MAX/4U)
                throw BpFault("scaled character-bitmap dimensions are invalid");
            const uint32_t alpha_width=(height*width_percent+50U)/100U;
            if(alpha_width>UINT32_MAX/4U || uint64_t(std::max(width,alpha_width))*canvas_height>SIZE_MAX/4U)
                throw BpFault("scaled character-bitmap alpha dimensions are invalid");
            const uint32_t alpha_stride=std::max(width,alpha_width);
            glyph.width=width;glyph.height=canvas_height;glyph.stride=alpha_stride;
            glyph.metrics.words[5]=alpha_width-1U;glyph.metrics.words[6]=height-1U;
            if(input.format==0) {
                // 52B50 accepts the call but dispatches no sf0/df2 worker,
                // including the identity-copy fast path through 4F3E0.
                glyph.bitmap.assign(size_t(width)*canvas_height*4U,0);
                glyph.alpha.assign(size_t(alpha_stride)*canvas_height,0);return glyph;
            }
            uint32_t sw=input.width,sh=input.height,stride=input.stride;
            std::vector<uint8_t> source_pixels=input.format==3?glyph.bitmap:input.pixels;
            if(input.format==3)stride=sw*4U;
            if(input.format==1)for(uint32_t y=0;y<sh;++y)for(uint32_t x=0;x<sw;++x)
                source_pixels[size_t(y)*stride+x*4U+3U]=255;
            BackgroundTransform transform{static_cast<int32_t>(sw<<15U),static_cast<int32_t>(sh<<15U),0,
                static_cast<int32_t>((height*width_percent<<16U)/(sw*100U)),
                static_cast<int32_t>((height<<16U)/sh)};
            if(transform.scale_x<0x8000 || transform.scale_y<0x8000) {
                const uint32_t rw=(sw+1U)/2U,rh=(sh+1U)/2U;
                std::vector<uint8_t> reduced(size_t(rw)*rh*4U);
                GlyphBitmapView from{source_pixels,0,stride,sw,sh,2,4},to{reduced,0,rw*4U,rw,rh,2,4};
                if(!reduce_original_sprite_cache(to,from))throw BpFault("scaled character-bitmap reduction failed");
                source_pixels=std::move(reduced);sw=rw;sh=rh;stride=rw*4U;
                transform.origin_x>>=1;transform.origin_y>>=1;
                transform.scale_x=static_cast<int32_t>(uint32_t(transform.scale_x)<<1U);
                transform.scale_y=static_cast<int32_t>(uint32_t(transform.scale_y)<<1U);
            }
            // Outline72AE0 redraws into an expanded canvas and bounds the
            // alpha scan by rounded font width, one column wider when needed.
            std::vector<uint8_t> expanded(size_t(alpha_stride)*canvas_height*4U,0);
            const auto matrix=evaluate_pe64_background_affine(transform,
                static_cast<int32_t>(height*width_percent<<16U)/200,static_cast<int32_t>(height<<15U));
            if(alpha_stride && canvas_height && (!matrix || !sample_background_transform(expanded.data(),alpha_stride*4U,alpha_stride,canvas_height,
                source_pixels.data(),stride,sw,sh,*matrix)))throw BpFault("scaled character-bitmap affine copy failed");
            glyph.bitmap.assign(size_t(width)*canvas_height*4U,0);
            if(width)for(uint32_t y=0;y<canvas_height;++y)std::copy_n(expanded.data()+size_t(y)*alpha_stride*4U,
                size_t(width)*4U,glyph.bitmap.data()+size_t(y)*width*4U);
            glyph.alpha.resize(size_t(alpha_stride)*canvas_height);
            for(size_t i=0;i<glyph.alpha.size();++i)glyph.alpha[i]=expanded[i*4U+3U];
        }
        if(!scaled && (padding_x || padding_y)) {
            const uint32_t width=input.width+padding_x,height=input.height+padding_y;
            if(width<input.width || height<input.height || width>UINT32_MAX/4U || uint64_t(width)*height>SIZE_MAX/4U)
                throw BpFault("private glyph style padding dimensions are invalid");
            std::vector<uint8_t> padded(size_t(width)*height*4U,0),alpha(size_t(width)*height,0);
            for(uint32_t y=0;y<input.height;++y)for(uint32_t x=0;x<input.width;++x) {
                std::copy_n(glyph.bitmap.data()+(size_t(y)*input.width+x)*4U,4U,padded.data()+(size_t(y)*width+x)*4U);
                alpha[size_t(y)*width+x]=glyph.alpha[size_t(y)*input.width+x];
            }
            glyph.bitmap=std::move(padded);glyph.alpha=std::move(alpha);
            glyph.width=width;glyph.height=height;glyph.stride=width;
        }
        return glyph;
    };
}
}
