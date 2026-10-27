#include "bgiplayer/glyph_style.hpp"
#include <algorithm>
#include <array>
#include <limits>
#include <cmath>

namespace bgiplayer {
namespace {
#include "glyph_outline_weights.inc"
constexpr std::array<std::size_t,5> weight_offsets{0,9,34,83,164};
bool valid_style_bitmap(const GlyphBitmapView& bitmap){
    if(bitmap.format!=2 || bitmap.bytes_per_pixel!=4 || bitmap.width>std::numeric_limits<uint32_t>::max()/4U ||
       bitmap.stride<bitmap.width*4U)return false;
    const uint64_t end=bitmap.height==0?bitmap.byte_offset:
        static_cast<uint64_t>(bitmap.byte_offset)+static_cast<uint64_t>(bitmap.height-1U)*bitmap.stride+
        static_cast<uint64_t>(bitmap.width)*4U;
    return end<=bitmap.pixels.size();
}
void write_style_pixel(const GlyphBitmapView& bitmap,uint32_t x,uint32_t y,uint32_t value){
    const auto offset=static_cast<std::size_t>(bitmap.byte_offset)+static_cast<std::size_t>(y)*bitmap.stride+
        static_cast<std::size_t>(x)*4U;
    for(unsigned byte=0;byte<4;++byte)bitmap.pixels[offset+byte]=static_cast<uint8_t>(value>>(byte*8U));
}
}
bool colorize_glyph_shadow(GlyphBitmapView destination,GlyphBitmapView source,uint32_t color){
    if(!valid_style_bitmap(destination) || !valid_style_bitmap(source) ||
       destination.width<source.width || destination.height<source.height)return false;
    for(uint32_t y=0;y<source.height;++y)for(uint32_t x=0;x<source.width;++x){
        const auto offset=static_cast<std::size_t>(source.byte_offset)+static_cast<std::size_t>(y)*source.stride+
            static_cast<std::size_t>(x)*4U;
        write_style_pixel(destination,x,y,(static_cast<uint32_t>(source.pixels[offset+3U])<<24U)|(color&0xFFFFFFU));
    }
    return true;
}
bool colorize_glyph_outline(GlyphBitmapView destination,const GlyphMask& glyph,
                           uint32_t radius_x,uint32_t radius_y,uint32_t color){
    if(radius_x<1 || radius_y<1 || radius_x>0x3fffffffU || radius_y>0x3fffffffU || !valid_style_bitmap(destination) ||
       glyph.stride<glyph.width)return false;
    const uint64_t end=glyph.height==0?0:static_cast<uint64_t>(glyph.height-1U)*glyph.stride+glyph.width;
    if(end>glyph.alpha.size())return false;
    const uint32_t side_x=radius_x*2U+1U,side_y=radius_y*2U+1U;
    const bool table=radius_x==radius_y && radius_x<=5U;
    const std::size_t weights=table?weight_offsets[radius_x-1U]:0U;
    for(uint32_t y=0;y<destination.height;++y)for(uint32_t x=0;x<destination.width;++x){
        uint32_t sum=0;
        for(uint32_t ky=0;ky<side_y;++ky){
            const int64_t sy=static_cast<int64_t>(y)+ky-radius_y*2U;
            if(sy<0 || sy>=glyph.height)continue;
            for(uint32_t kx=0;kx<side_x;++kx){
                const int64_t sx=static_cast<int64_t>(x)+kx-radius_x*2U;
                if(sx<0 || sx>=glyph.width)continue;
                const auto alpha=glyph.alpha[static_cast<std::size_t>(sy)*glyph.stride+static_cast<std::size_t>(sx)];
                if(table) {
                    const uint32_t weight=outline_weights[weights+static_cast<std::size_t>(ky)*side_x+kx];
                    sum+=(static_cast<uint32_t>(alpha)*weight)>>16U;
                } else {
                    // 14003F6B3 / 0040A020 general ellipse branch. Its edge
                    // factor is distance-minus-radius, not one-minus-distance.
                    const double dx=(static_cast<double>(kx)-radius_x)*radius_y/radius_x;
                    const int64_t dy=static_cast<int64_t>(ky)-radius_y;
                    const double edge=std::sqrt(dx*dx+static_cast<double>(dy*dy))-radius_y;
                    if(edge<=0.0)sum+=alpha;
                    else if(edge<1.0)sum+=static_cast<uint32_t>(alpha*edge);
                }
            }
        }
        write_style_pixel(destination,x,y,(std::min(sum,255U)<<24U)|color);
    }
    return true;
}
bool colorize_pe64_gaiji_outline(GlyphBitmapView destination,const GlyphMask& glyph,
                           uint32_t radius_x,uint32_t radius_y,uint32_t color){
    if(radius_x<1 || radius_y<1 || radius_x>0x3fffffffU || radius_y>0x3fffffffU || !valid_style_bitmap(destination) ||
       glyph.stride<glyph.width)return false;
    const uint64_t end=glyph.height==0?0:static_cast<uint64_t>(glyph.height-1U)*glyph.stride+glyph.width;
    if(end>glyph.alpha.size())return false;
    const uint32_t side_x=radius_x*2U+1U,side_y=radius_y*2U+1U;
    for(uint32_t y=0;y<destination.height;++y)for(uint32_t x=0;x<destination.width;++x){
        uint32_t sum=0;
        for(uint32_t ky=0;ky<side_y;++ky){
            const int64_t sy=static_cast<int64_t>(y)+ky-radius_y*2U;
            if(sy<0 || sy>=glyph.height)continue;
            for(uint32_t kx=0;kx<side_x;++kx){
                const int64_t sx=static_cast<int64_t>(x)+kx-radius_x*2U;
                if(sx<0 || sx>=glyph.width)continue;
                const auto alpha=glyph.alpha[static_cast<std::size_t>(sy)*glyph.stride+static_cast<std::size_t>(sx)];
                sum+=alpha;
            }
        }
        write_style_pixel(destination,x,y,(std::min(sum,255U)<<24U)|color);
    }
    return true;
}
} // namespace bgiplayer
