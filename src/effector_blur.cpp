#include "bgiplayer/effector_blur.hpp"
#include <algorithm>
#include <array>
#include <cstring>
namespace bgiplayer {
namespace {
bool valid(const GlyphBitmapView& v){return (v.format==1||v.format==2)&&v.bytes_per_pixel==4&&
    v.width!=0&&v.height!=0&&v.stride>=uint64_t(v.width)*4&&
    uint64_t(v.byte_offset)+uint64_t(v.height-1)*v.stride+uint64_t(v.width)*4<=v.pixels.size();}
}
bool blur_pe64_effector(GlyphBitmapView dst,GlyphBitmapView src,uint32_t radius){
    if(!valid(dst)||!valid(src)||dst.format!=src.format||dst.width!=src.width||dst.height!=src.height||radius>256)return false;
    if(radius==0){for(uint32_t y=0;y<src.height;++y)std::memmove(dst.pixels.data()+dst.byte_offset+uint64_t(y)*dst.stride,
        src.pixels.data()+src.byte_offset+uint64_t(y)*src.stride,uint64_t(src.width)*4);return true;}
    radius=std::min(radius,255U);
    const uint32_t diameter=radius*2+1;
    const uint32_t reciprocal=((0x20000U+diameter*2)/diameter)&0xffffU;
    std::vector<uint8_t> horizontal(uint64_t(src.width)*src.height*4);
    const auto sx=[&](int64_t x){return uint32_t(std::clamp<int64_t>(x,0,src.width-1));};
    const auto sy=[&](int64_t y){return uint32_t(std::clamp<int64_t>(y,0,src.height-1));};
    for(uint32_t y=0;y<src.height;++y){
        std::array<uint32_t,4> sums{};
        const auto add=[&](int64_t x,int sign){const auto i=src.byte_offset+uint64_t(y)*src.stride+uint64_t(sx(x))*4;
            for(unsigned c=0;c<4;++c)sums[c]+=sign*int32_t(src.pixels[i+c]>>1);};
        for(int64_t x=-int64_t(radius);x<=radius;++x)add(x,1);
        for(uint32_t x=0;x<src.width;++x){
            for(unsigned c=0;c<4;++c)horizontal[(uint64_t(y)*src.width+x)*4+c]=uint8_t((sums[c]*reciprocal)>>16);
            add(int64_t(x)-radius,-1);add(int64_t(x)+radius+1,1);
        }
    }
    for(uint32_t x=0;x<src.width;++x){
        std::array<uint32_t,4> sums{};
        const auto add=[&](int64_t y,int sign){const auto i=(uint64_t(sy(y))*src.width+x)*4;
            for(unsigned c=0;c<4;++c)sums[c]+=sign*int32_t(horizontal[i+c]>>1);};
        for(int64_t y=-int64_t(radius);y<=radius;++y)add(y,1);
        for(uint32_t y=0;y<src.height;++y){
            const auto i=dst.byte_offset+uint64_t(y)*dst.stride+uint64_t(x)*4;
            for(unsigned c=0;c<4;++c)dst.pixels[i+c]=uint8_t((sums[c]*reciprocal)>>16);
            add(int64_t(y)-radius,-1);add(int64_t(y)+radius+1,1);
        }
    }
    return true;
}
}
