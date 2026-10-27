#include "bgiplayer/sprite_rotated_alpha.hpp"
#include "bgiplayer/gpu_pixel_kernels.hpp"
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#pragma STDC FP_CONTRACT OFF
namespace bgiplayer {
namespace {
int32_t truncate_sse(double value) {
    if (!(value >= -2147483648.0 && value < 2147483648.0))
        return std::numeric_limits<int32_t>::min();
    return static_cast<int32_t>(value);
}
int32_t add_wrap(int32_t lhs,int32_t rhs){
    return std::bit_cast<int32_t>(uint32_t(lhs)+uint32_t(rhs));
}
int floor_shift(int value,unsigned shift){
    const int divisor=1<<shift;
    return value>=0?value/divisor:-int((-int64_t(value)+divisor-1)/divisor);
}
}
std::optional<std::array<int32_t,6>> inverse_sprite_affine_q16(int32_t xq,int32_t yq,
    int32_t oxq,int32_t oyq,int32_t angleq,uint32_t sxq,uint32_t syq){
    if(sxq==0||syq==0)return std::nullopt;
    // Original52700 literal operation order, including theta+pi/2 trig calls.
    const double theta=-(double(angleq)*3.141592653589793/11796480.0);
    const double c=std::cos(theta),sn=std::sin(theta);
    const double inv_x=65536.0/double(sxq),inv_y=65536.0/double(syq);
    const double nx=-(double(xq)/65536.0),yy=double(yq)/65536.0;
    const volatile double first_x=nx*c,second_x=yy*sn;
    const volatile double first_y=nx*sn,second_y=yy*c;
    const volatile double origin_x=(first_x-second_x)*inv_x;
    const volatile double origin_y=(first_y+second_y)*inv_y;
    const int32_t row_x=truncate_sse((origin_x+double(oxq)/65536.0)*65536.0);
    const int32_t row_y=truncate_sse((double(oyq)/65536.0-origin_y)*65536.0);
    const int32_t col_x=truncate_sse((inv_x*c)*65536.0);
    const int32_t col_y=truncate_sse((inv_y*sn)*-65536.0);
    const double turned=theta+1.5707963267948966;
    const int32_t line_y=truncate_sse((std::sin(turned)*inv_y)*65536.0);
    const int32_t line_x=truncate_sse((std::cos(turned)*inv_x)*-65536.0);
    return std::array<int32_t,6>{row_x,row_y,col_x,col_y,line_x,line_y};
}
bool compose_rotated_alpha_bgra(std::span<uint8_t> destination,uint32_t dw,uint32_t dh,uint32_t ds,
    std::span<const uint8_t> source,uint32_t sw,uint32_t sh,uint32_t ss,
    int32_t xq,int32_t yq,int32_t oxq,int32_t oyq,int32_t angleq,
    uint32_t sxq,uint32_t syq,uint32_t parameter,bool filtered){
    if(sxq==0||syq==0||parameter>256||sw>32767||sh>32767||dw>32767||dh>32767||
        ds%4||ss%4||ss>32764||ds>2147483647U||ds<uint64_t(dw)*4||ss<uint64_t(sw)*4||
        destination.size()<uint64_t(ds)*dh||source.size()<uint64_t(ss)*sh)return false;
    const auto matrix=inverse_sprite_affine_q16(xq,yq,oxq,oyq,angleq,sxq,syq);
    if(compose_host_fast_affine({destination,dw,dh,ds,source,sw,sh,ss,
            *matrix,parameter,filtered,true}).success)return true;
    int32_t row_x=(*matrix)[0],row_y=(*matrix)[1];
    const int32_t col_x=(*matrix)[2],col_y=(*matrix)[3],line_x=(*matrix)[4],line_y=(*matrix)[5];
    const auto sample=[&](int x,int y,unsigned channel)->int{
        if(x<0||y<0||uint32_t(x)>=sw||uint32_t(y)>=sh)return 0;
        return source[size_t(y)*ss+size_t(x)*4+channel];
    };
    if(!filtered){row_x=add_wrap(row_x,32768);row_y=add_wrap(row_y,32768);}
    for(uint32_t y=0;y<dh;++y){
        int32_t qx=row_x,qy=row_y;
        for(uint32_t x=0;x<dw;++x){
            const int ix=floor_shift(qx,16),iy=floor_shift(qy,16);
            std::array<int,4> rgba{};
            if(filtered){
                const int fx=int((uint32_t(qx)&65535U)>>12),fy=int((uint32_t(qy)&65535U)>>12);
                for(unsigned ch=0;ch<4;++ch){
                    const int tl=sample(ix,iy,ch),bl=sample(ix,iy+1,ch);
                    const int top=tl+floor_shift((sample(ix+1,iy,ch)-tl)*fx,4);
                    const int bottom=bl+floor_shift((sample(ix+1,iy+1,ch)-bl)*fx,4);
                    rgba[ch]=top+floor_shift((bottom-top)*fy,4);
                }
            }else for(unsigned ch=0;ch<4;++ch)rgba[ch]=sample(ix,iy,ch);
            const int index=rgba[3]>>1;
            const int weight=((index==127?128:index)*int(256-parameter))>>8;
            const size_t offset=size_t(y)*ds+size_t(x)*4;
            for(unsigned ch=0;ch<3;++ch){const int old=destination[offset+ch];
                destination[offset+ch]=uint8_t(old+floor_shift((rgba[ch]-old)*weight,7));}
            qx=add_wrap(qx,col_x);qy=add_wrap(qy,col_y);
        }
        row_x=add_wrap(row_x,line_x);row_y=add_wrap(row_y,line_y);
    }
    return true;
}
}
