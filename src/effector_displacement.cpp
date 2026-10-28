#include "bgiplayer/effector_displacement.hpp"
#include <algorithm>
#include <cstring>
namespace bgiplayer {
namespace {
bool valid(const GlyphBitmapView& v) {
    return v.bytes_per_pixel==4&&v.width&&v.height&&uint64_t(v.stride)>=uint64_t(v.width)*4&&
        uint64_t(v.byte_offset)+uint64_t(v.height-1)*v.stride+uint64_t(v.width)*4<=v.pixels.size();
}
int16_t word(const GlyphBitmapView& v,uint32_t x,uint32_t y,unsigned component){
    const auto i=v.byte_offset+uint64_t(y)*v.stride+uint64_t(x)*4+component*2;
    return int16_t(uint16_t(v.pixels[i])|(uint16_t(v.pixels[i+1])<<8));
}
int16_t wrap(int32_t value){return int16_t(uint16_t(value));}
int32_t high(int32_t lhs,int32_t rhs){return (lhs*rhs)>>16;}
}
bool displace_pe64_effector(GlyphBitmapView dst,GlyphBitmapView src,
                           GlyphBitmapView primary,const GlyphBitmapView* secondary,
                           uint32_t strength,uint32_t bilinear){
    if(!valid(dst)||!valid(src)||!valid(primary)||strength>256||
       dst.format!=src.format||(src.format!=1&&src.format!=2)||primary.format!=4||
       dst.width>primary.width||dst.height>primary.height||dst.width!=src.width||dst.height!=src.height||
       (secondary&&(!valid(*secondary)||secondary->format!=4||secondary->width!=primary.width||secondary->height!=primary.height)))return false;
    if(!secondary&&strength==0){
        for(uint32_t y=0;y<src.height;++y)std::memcpy(dst.pixels.data()+dst.byte_offset+uint64_t(y)*dst.stride,
            src.pixels.data()+src.byte_offset+uint64_t(y)*src.stride,uint64_t(src.width)*4);
        return true;
    }
    const bool dual=secondary&&strength>0&&strength<256;
    const auto& map=secondary&&strength==256?*secondary:primary;
    const uint32_t effective=secondary&&(strength==0||strength==256)?256:strength;
    const unsigned bits=bilinear?(dual||effective<256?3:4):0;
    const auto sample=[&](int32_t x,int32_t y,unsigned channel)->int32_t{
        if(x<0||y<0||uint32_t(x)>=src.width||uint32_t(y)>=src.height)return 0;
        return src.pixels[src.byte_offset+uint64_t(y)*src.stride+uint64_t(x)*4+channel];
    };
    for(uint32_t y=0;y<dst.height;++y)for(uint32_t x=0;x<dst.width;++x){
        int32_t displacement[2];
        for(unsigned component=0;component<2;++component){
            const auto a=word(map,x,y,component);
            if(dual){
                const auto difference=wrap(int32_t(word(*secondary,x,y,component))-a);
                displacement[component]=wrap((a>>1)+high(difference,int32_t(strength<<7)));
                if(!bilinear)displacement[component]=wrap(displacement[component]+4)>>3;
            }else if(bilinear){
                displacement[component]=effective==256?a:high(a,int32_t(effective<<7));
            }else{
                displacement[component]=(int32_t(a)*int32_t(effective)+8)>>12;
            }
        }
        int32_t sx,sy,fx=0,fy=0;
        if(bilinear){
            const int32_t mask=(1<<bits)-1;
            fx=displacement[0]&mask;fy=displacement[1]&mask;
            sx=wrap(int32_t(x)+(displacement[0]>>bits));sy=wrap(int32_t(y)+(displacement[1]>>bits));
        }else{sx=wrap(int32_t(x)+displacement[0]);sy=wrap(int32_t(y)+displacement[1]);}
        const auto offset=dst.byte_offset+uint64_t(y)*dst.stride+uint64_t(x)*4;
        for(unsigned channel=0;channel<4;++channel){
            int32_t value=sample(sx,sy,channel);
            if(bilinear){
                const int32_t top=value+((sample(sx+1,sy,channel)-value)*fx>>bits);
                const int32_t bottom0=sample(sx,sy+1,channel);
                const int32_t bottom=bottom0+((sample(sx+1,sy+1,channel)-bottom0)*fx>>bits);
                value=top+((bottom-top)*fy>>bits);
            }
            dst.pixels[offset+channel]=uint8_t(std::clamp(value,0,255));
        }
    }
    return true;
}
}
