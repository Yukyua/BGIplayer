#include "bgiplayer/effector_temporal.hpp"
#include "bgiplayer/rcpss.hpp"
#pragma STDC FP_CONTRACT OFF
namespace bgiplayer {
bool temporal_pe64_effector(GlyphBitmapView scene,GlyphBitmapView history,uint32_t strength){
    const auto valid=[](const GlyphBitmapView& v){return v.bytes_per_pixel==4&&v.width&&v.height&&
        (v.format==1||v.format==2)&&uint64_t(v.stride)>=uint64_t(v.width)*4&&
        uint64_t(v.byte_offset)+uint64_t(v.height-1)*v.stride+uint64_t(v.width)*4<=v.pixels.size();};
    if(!valid(scene)||!valid(history)||scene.format!=history.format ||
       history.width>scene.width||history.height>scene.height ||
       uint64_t(history.byte_offset)+uint64_t(scene.height-1)*history.stride+uint64_t(scene.width)*4>history.pixels.size())return false;
    // 5BDDD crops only history. Blend traverses its source descriptor;41790
    // subsequently copies the complete scene back, bounded by host storage.
    if(strength && strength<=256)for(uint32_t y=0;y<history.height;++y)for(uint32_t x=0;x<history.width;x+=2) {
        const uint32_t count=history.width-x>=2?2:1;
        const auto hi=history.byte_offset+uint64_t(y)*history.stride+uint64_t(x)*4;
        const auto si=scene.byte_offset+uint64_t(y)*scene.stride+uint64_t(x)*4;
        if(scene.format==2 && history.pixels[hi+3]==0 && (count==1||history.pixels[hi+7]==0))continue;
        for(uint32_t p=0;p<count;++p) {
            const auto h=hi+p*4,s=si+p*4;
            if(strength==256 && (scene.format==1 || (history.pixels[h+3]==255 && (count==1 || (history.pixels[hi+3]==255 && history.pixels[hi+7]==255)))))for(unsigned c=0;c<4;++c)scene.pixels[s+c]=history.pixels[h+c];
            else if(scene.format==1)for(unsigned c=0;c<4;++c)
                scene.pixels[s+c]=uint8_t(int32_t(history.pixels[h+c])+((int32_t(scene.pixels[s+c])-int32_t(history.pixels[h+c]))*int32_t((256-strength)>>1)>>7));
            else {
                const uint32_t ha=history.pixels[h+3],sa=scene.pixels[s+3];uint32_t hw,sw,alpha;
                if(count==2) {
                    // 3D140 pairs: retain RCPPS estimate and each MULPS/ADDPS
                    // rounding; its odd tail uses independent integer DIVs.
                    const volatile float first=float(ha)*(float(strength)/256.0F);
                    const volatile float second=(float(sa)/256.0F)*(256.0F-first);
                    const float total=first+second,reciprocal=detail::rcpss_estimate(total==0?1.0F:total);
                    const volatile float a=reciprocal*first,b=reciprocal*second;
                    hw=static_cast<uint32_t>(a*256.0F);sw=static_cast<uint32_t>(b*256.0F);alpha=static_cast<uint32_t>(total);
                } else {
                    const uint32_t first=ha*strength,second=(sa*(65536U-first))>>8,total=first+second;
                    if(!total) {hw=sw=alpha=0;}else{hw=(first<<8)/total;sw=(second<<8)/total;alpha=total>>8;}
                }
                for(unsigned c=0;c<3;++c)scene.pixels[s+c]=uint8_t(((history.pixels[h+c]*hw+scene.pixels[s+c]*sw)&0xFFFFU)>>8);
                scene.pixels[s+3]=uint8_t(alpha);
            }
        }
    }
    for(uint32_t y=0;y<scene.height;++y)for(uint32_t x=0;x<scene.width*4;++x)
        history.pixels[history.byte_offset+uint64_t(y)*history.stride+x]=scene.pixels[scene.byte_offset+uint64_t(y)*scene.stride+x];
    return true;
}
}
