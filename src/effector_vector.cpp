#include "bgiplayer/effector_vector.hpp"
#include <algorithm>
namespace bgiplayer {
namespace {
bool valid(const GlyphBitmapView& v,uint32_t bpp){return v.bytes_per_pixel==bpp&&v.width&&v.height&&
    uint64_t(v.stride)>=uint64_t(v.width)*bpp&&uint64_t(v.byte_offset)+uint64_t(v.height-1)*v.stride+uint64_t(v.width)*bpp<=v.pixels.size();}
uint16_t word(const GlyphBitmapView& v,uint64_t i){return uint16_t(v.pixels[i])|(uint16_t(v.pixels[i+1])<<8);}
int32_t wrap(int32_t x){return int16_t(uint16_t(x));}
}
bool vector_pe64_effector(GlyphBitmapView dst,GlyphBitmapView src,GlyphBitmapView map,
                          std::span<const uint32_t> coefficients,uint32_t bilinear,bool rectangular_bounds){
    if(!valid(dst,4)||!valid(src,4)||!valid(map,6)||dst.format!=src.format||
       (src.format!=1&&src.format!=2)||map.format!=6||dst.width!=src.width||dst.height!=src.height||
       map.width!=dst.width||map.height!=dst.height)return false;
    // Validate the complete index map before writing any destination byte.
    for(uint32_t y=0;y<dst.height;++y)for(uint32_t x=0;x<dst.width;++x)
        if(word(map,map.byte_offset+uint64_t(y)*map.stride+uint64_t(x)*6+4)>=coefficients.size())return false;
    const auto sample=[&](int32_t x,int32_t y,unsigned c)->int32_t{
        if(rectangular_bounds) return x<0||y<0||uint32_t(x)>=src.width||uint32_t(y)>=src.height?0:
            src.pixels[src.byte_offset+uint64_t(y)*src.stride+uint64_t(x)*4+c];
        const int64_t index=int64_t(y)*(src.stride/4)+x;
        if(index<0||uint64_t(index)*4+c>=uint64_t(src.stride)*src.height)return 0;
        return src.pixels[src.byte_offset+uint64_t(index)*4+c];};
    for(uint32_t y=0;y<dst.height;++y)for(uint32_t x=0;x<dst.width;++x){
        const auto mi=map.byte_offset+uint64_t(y)*map.stride+uint64_t(x)*6;
        const uint32_t coefficient=coefficients[word(map,mi+4)];
        const int32_t qx=int16_t(word(map,mi))*int32_t(int16_t(coefficient));
        const int32_t qy=int16_t(word(map,mi+2))*int32_t(int16_t(coefficient>>16));
        const int32_t sx=wrap(int32_t(x)+((int64_t(qx)+(bilinear?0:32768))>>16));
        const int32_t sy=wrap(int32_t(y)+((int64_t(qy)+(bilinear?0:32768))>>16));
        const auto di=dst.byte_offset+uint64_t(y)*dst.stride+uint64_t(x)*4;
        for(unsigned c=0;c<4;++c){
            int32_t value=sample(sx,sy,c);
            if(bilinear){const auto fx=(uint32_t(qx)>>12)&15,fy=(uint32_t(qy)>>12)&15;
                const int32_t top=value+((sample(sx+1,sy,c)-value)*int32_t(fx)>>4);
                const int32_t bl=sample(sx,sy+1,c);
                const int32_t bottom=bl+((sample(sx+1,sy+1,c)-bl)*int32_t(fx)>>4);
                value=top+((bottom-top)*int32_t(fy)>>4);}
            dst.pixels[di+c]=uint8_t(std::clamp(value,0,255));
        }
    }
    return true;
}
}
