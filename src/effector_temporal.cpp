#include "bgiplayer/effector_temporal.hpp"
namespace bgiplayer {
bool temporal_pe64_effector(GlyphBitmapView scene,GlyphBitmapView history,uint32_t strength){
    const auto valid=[](const GlyphBitmapView& v){return v.bytes_per_pixel==4&&v.width&&v.height&&
        (v.format==1||v.format==2)&&uint64_t(v.stride)>=uint64_t(v.width)*4&&
        uint64_t(v.byte_offset)+uint64_t(v.height-1)*v.stride+uint64_t(v.width)*4<=v.pixels.size();};
    if(!valid(scene)||!valid(history)||scene.width!=history.width||scene.height!=history.height||scene.format!=history.format)return false;
    for(uint32_t y=0;y<scene.height;++y)for(uint32_t x=0;x<scene.width*4;++x){
        auto& s=scene.pixels[scene.byte_offset+uint64_t(y)*scene.stride+x];
        auto& h=history.pixels[history.byte_offset+uint64_t(y)*history.stride+x];
        if(strength<=256){
            if(strength==0){} //3DCE0's strength256 endpoint preserves current scene.
            else if(strength==256)s=h;
            else s=uint8_t(int32_t(h)+((int32_t(s)-int32_t(h))*int32_t((256-strength)>>1)>>7));
        }
        h=s;
    }
    return true;
}
}
