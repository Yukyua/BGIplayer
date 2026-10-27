#include "bgiplayer/glyph_opacity.hpp"
#include <algorithm>
#include "bgiplayer/rcpss.hpp"
#pragma STDC FP_CONTRACT OFF
namespace bgiplayer {
namespace {
bool valid(const GlyphBitmapView& view){return view.format==2 && view.bytes_per_pixel==4 &&
    view.width<=UINT32_MAX/4U && view.stride>=view.width*4U &&
    static_cast<uint64_t>(view.byte_offset)+(view.height==0?0:static_cast<uint64_t>(view.height-1U)*view.stride+view.width*4U)<=view.pixels.size();}
}
bool blend_original_glyph_opacity(GlyphBitmapView dst,GlyphBitmapView src,int32_t x,int32_t y,uint32_t parameter){
    if(!valid(dst)||!valid(src)||parameter==0||parameter>=256)return false;
    const int64_t left=std::max<int64_t>(x,0),top=std::max<int64_t>(y,0);
    const int64_t right=std::min<int64_t>(static_cast<int64_t>(x)+src.width, dst.width);
    const int64_t bottom=std::min<int64_t>(static_cast<int64_t>(y)+src.height,dst.height);
    for(int64_t row=top;row<bottom;++row)for(int64_t column=left;column<right;column+=2){
        const uint32_t count=column+1<right?2U:1U;
        const auto so=src.byte_offset+static_cast<std::size_t>(row-y)*src.stride+static_cast<std::size_t>(column-x)*4U;
        const auto output=dst.byte_offset+static_cast<std::size_t>(row)*dst.stride+static_cast<std::size_t>(column)*4U;
        if(src.pixels[so+3]==0 && (count==1||src.pixels[so+7]==0))continue;
        for(uint32_t pixel=0;pixel<count;++pixel){
            const auto si=so+pixel*4U,di=output+pixel*4U;
            const uint32_t alpha=src.pixels[si+3],da=dst.pixels[di+3];
            uint32_t sw,dw,out_alpha;
            if(count==2){
                const float sf=static_cast<float>(alpha)*(static_cast<float>(256U-parameter)/256.0F);
                const float df=(static_cast<float>(da)/256.0F)*(256.0F-sf);
                const float total=sf+df;
                if(total==0){sw=0;dw=0;out_alpha=0;}
                else{const float reciprocal=detail::rcpss_estimate(total);
                    sw=static_cast<uint32_t>((reciprocal*sf)*256.0F);
                    dw=static_cast<uint32_t>((reciprocal*df)*256.0F);
                    out_alpha=static_cast<uint32_t>(total);}
            }else{
                const uint32_t scaled=alpha*(256U-parameter),dc=(da*(65536U-scaled))>>8U,total=scaled+dc;
                sw=(scaled<<8U)/total;dw=(dc<<8U)/total;out_alpha=total>>8U;
            }
            for(uint32_t byte=0;byte<3;++byte)
                dst.pixels[di+byte]=static_cast<uint8_t>(((src.pixels[si+byte]*sw+dst.pixels[di+byte]*dw)&65535U)>>8U);
            dst.pixels[di+3]=static_cast<uint8_t>(out_alpha);
        }
    }
    return true;
}
}
