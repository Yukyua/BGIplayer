#include "bgiplayer/effector_transform.hpp"
#include "bgiplayer/sprite_rotated_alpha.hpp"
#include <algorithm>
namespace bgiplayer {
std::optional<std::array<int32_t,6>> effector_transform_matrix(
    uint32_t width,uint32_t height,const std::array<int32_t,5>& transform) {
    if(!transform[3] || !transform[4])return std::nullopt;
    const auto x=static_cast<int32_t>((width<<15U)&0xFFFF0000U);
    const auto y=static_cast<int32_t>((height<<15U)&0xFFFF0000U);
    return inverse_sprite_affine_q16(x,y,transform[0],transform[1],transform[2],
        static_cast<uint32_t>(transform[3]),static_cast<uint32_t>(transform[4]));
}
bool transform_pe64_effector(GlyphBitmapView destination,GlyphBitmapView source,
                             const std::array<int32_t,5>& transform,uint32_t bilinear) {
    const auto valid=[](const GlyphBitmapView& v){return v.bytes_per_pixel==4 && v.width && v.height &&
        v.stride>=uint64_t(v.width)*4 && uint64_t(v.byte_offset)+uint64_t(v.height-1)*v.stride+uint64_t(v.width)*4<=v.pixels.size();};
    if(!valid(destination) || !valid(source))return false;
    if(!bilinear && (source.width>32767U || source.height>32767U ||
                     source.stride>131068U || (source.stride&3U)))return false;
    if(destination.format!=source.format || (source.format!=1 && source.format!=2))return true;
    // 52B50 rejects zero scales; callback5BD50 intentionally ignores the status.
    if(!transform[3] || !transform[4])return true;
    const auto matrix=effector_transform_matrix(source.width,source.height,transform);
    if(!matrix)return false;
    const auto& m=*matrix;
    const auto sample=[&](int32_t x,int32_t y,unsigned c)->int32_t {
        if(x<0||y<0||uint32_t(x)>=source.width||uint32_t(y)>=source.height)return 0;
        return source.pixels[source.byte_offset+uint64_t(y)*source.stride+uint64_t(x)*4+c];
    };
    // Keep the CPU reference independent of the host's optional fast-affine
    // path. Q16 coordinates wrap before the nearest or Q4 bilinear sampling.
    for(uint32_t y=0;y<destination.height;++y)for(uint32_t x=0;x<destination.width;++x) {
        const uint32_t fx=uint32_t(m[0])+x*uint32_t(m[2])+y*uint32_t(m[4])+(bilinear?0U:0x8000U);
        const uint32_t fy=uint32_t(m[1])+x*uint32_t(m[3])+y*uint32_t(m[5])+(bilinear?0U:0x8000U);
        const int32_t sx=int32_t(fx)>>16,sy=int32_t(fy)>>16;
        const int32_t ax=(fx>>12)&15,ay=(fy>>12)&15;
        const auto offset=destination.byte_offset+uint64_t(y)*destination.stride+uint64_t(x)*4;
        for(unsigned c=0;c<4;++c) {
            int32_t value=sample(sx,sy,c);
            if(bilinear) {
                const int32_t top=value+((sample(sx+1,sy,c)-value)*ax>>4);
                const int32_t bottom0=sample(sx,sy+1,c);
                const int32_t bottom=bottom0+((sample(sx+1,sy+1,c)-bottom0)*ax>>4);
                value=top+((bottom-top)*ay>>4);
            }
            destination.pixels[offset+c]=static_cast<uint8_t>(std::clamp(value,0,255));
        }
    }
    return true;
}
}
