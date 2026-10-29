#include "bgiplayer/gpu_pixel_kernels.hpp"
#include "bgiplayer/debug.hpp"
#include "bgiplayer/sprite_scaled_alpha.hpp"
#include "bgiplayer/sprite_crossfade.hpp"
#include "bgiplayer/rcpss_estimate_table.hpp"
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <algorithm>
#include <bit>
#include <chrono>
#include <cstring>
#include <limits>
#include <iostream>

namespace bgiplayer {
namespace {
using Clock=std::chrono::steady_clock;
uint64_t elapsed(Clock::time_point start) {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-start).count());
}
int32_t truncate_sse(double v) {
    return v>=-2147483648.0 && v<2147483648.0 ? static_cast<int32_t>(v) : std::numeric_limits<int32_t>::min();
}
struct Parameters {
    uint32_t dw,dh,ds,sw,sh,ss,start_x,start_y,dx,dy,parameter,filtered;
};
Parameters parameters(const ScaledAlphaPixels& p) {
    const double invx=65536.0/double(p.scale_x_q16), invy=65536.0/double(p.scale_y_q16);
    const volatile double tx=double(p.x_q16)/65536.0*invx, ty=double(p.y_q16)/65536.0*invy;
    const int32_t x=truncate_sse((double(p.origin_x_q16)/65536.0-tx)*65536.0);
    const int32_t y=truncate_sse((double(p.origin_y_q16)/65536.0-ty)*65536.0);
    const uint32_t bias=p.filtered?0U:32768U;
    return {p.destination_width,p.destination_height,p.destination_stride,p.source_width,
        p.source_height,p.source_stride,uint32_t(x)+bias,uint32_t(y)+bias,
        uint32_t(truncate_sse(invx*65536.0)),uint32_t(truncate_sse(invy*65536.0)),
        p.parameter,p.filtered?1U:0U};
}
bool supported(const ScaledAlphaPixels& p) {
    if(p.scale_x_q16<=32768 || p.scale_y_q16<=32768 || p.parameter>256 ||
       p.source_width>32767 || p.source_height>32767 || p.destination_width>32767 ||
       p.destination_height>32767 || p.destination_stride%4 || p.source_stride%4 ||
       p.source_stride>32764 || p.destination_stride>2147483647U ||
       p.destination_stride<uint64_t(p.destination_width)*4 || p.source_stride<uint64_t(p.source_width)*4 ||
       p.destination.size()<uint64_t(p.destination_stride)*p.destination_height ||
       p.source.size()<uint64_t(p.source_stride)*p.source_height) return false;
    const uintptr_t d=reinterpret_cast<uintptr_t>(p.destination.data()), s=reinterpret_cast<uintptr_t>(p.source.data());
    return d>=s+p.source.size() || s>=d+p.destination.size();
}
const char* shader_source=R"MSL(
#include <metal_stdlib>
using namespace metal;
struct Blur { uint ds,ss,d0,s0,width,height,radius,reciprocal,selector; };
uint blur_horizontal_sample(device const uchar* src,constant Blur& p,int x,uint y,uint c) {
    if((p.selector&1u)==0u && (x<0 || x>=int(p.width)))return 0;
    return uint(src[p.s0+y*p.ss+uint(clamp(x,0,int(p.width)-1))*4+c])>>1;
}
uint blur_vertical_sample(device const uchar* src,constant Blur& p,int y,uint x,uint c) {
    if((p.selector&1u)==0u && (y<0 || y>=int(p.height)))return 0;
    uint row=uint(clamp(y,0,int(p.height)-1));
    uint at=p.selector<4u?p.s0+row*p.ss+x*4:(row*p.width+x)*4;
    return uint(src[at+c])>>1;
}
// One lane scans a line, preserving the original sliding sums, byte halves,
// fixed reciprocal and intermediate byte truncation in both passes.
kernel void effector_horizontal(device uchar* tmp [[buffer(0)]],device const uchar* src [[buffer(1)]],
    constant Blur& p [[buffer(2)]],uint y [[thread_position_in_grid]]) {
    if(y>=p.height)return;
    uint4 sums=uint4(0);
    for(int x=-int(p.radius);x<=int(p.radius);++x)
        for(uint c=0;c<4;++c)sums[c]+=blur_horizontal_sample(src,p,x,y,c);
    for(uint x=0;x<p.width;++x) {
        uint at=p.selector<2u?p.d0+y*p.ds+x*4:(y*p.width+x)*4;
        for(uint c=0;c<4;++c)tmp[at+c]=uchar((sums[c]*p.reciprocal)>>16);
        for(uint c=0;c<4;++c)sums[c]+=blur_horizontal_sample(src,p,int(x+p.radius+1),y,c)-blur_horizontal_sample(src,p,int(x)-int(p.radius),y,c);
    }
}
kernel void effector_vertical(device uchar* dst [[buffer(0)]],device const uchar* tmp [[buffer(1)]],
    constant Blur& p [[buffer(2)]],uint x [[thread_position_in_grid]]) {
    if(x>=p.width)return;
    uint4 sums=uint4(0);
    for(int y=-int(p.radius);y<=int(p.radius);++y)
        for(uint c=0;c<4;++c)sums[c]+=blur_vertical_sample(tmp,p,y,x,c);
    for(uint y=0;y<p.height;++y) {
        for(uint c=0;c<4;++c)dst[p.d0+y*p.ds+x*4+c]=uchar((sums[c]*p.reciprocal)>>16);
        for(uint c=0;c<4;++c)sums[c]+=blur_vertical_sample(tmp,p,int(y+p.radius+1),x,c)-blur_vertical_sample(tmp,p,int(y)-int(p.radius),x,c);
    }
}
// Small radii expose pixels rather than complete scanlines to the GPU. The
// integer sums and the quantized intermediate remain identical to the sliding
// window above; large radii keep its radius-independent work per line.
kernel void effector_horizontal_pixels(device uchar* dst [[buffer(0)]],device const uchar* src [[buffer(1)]],
    constant Blur& p [[buffer(2)]],uint2 t [[thread_position_in_grid]]) {
    if(t.x>=p.width || t.y>=p.height)return;
    uint4 sums=uint4(0);
    for(int dx=-int(p.radius);dx<=int(p.radius);++dx)
        for(uint c=0;c<4;++c)sums[c]+=blur_horizontal_sample(src,p,int(t.x)+dx,t.y,c);
    uint at=p.selector<2u?p.d0+t.y*p.ds+t.x*4:(t.y*p.width+t.x)*4;
    for(uint c=0;c<4;++c)dst[at+c]=uchar((sums[c]*p.reciprocal)>>16);
}
kernel void effector_vertical_pixels(device uchar* dst [[buffer(0)]],device const uchar* src [[buffer(1)]],
    constant Blur& p [[buffer(2)]],uint2 t [[thread_position_in_grid]]) {
    if(t.x>=p.width || t.y>=p.height)return;
    uint4 sums=uint4(0);
    for(int dy=-int(p.radius);dy<=int(p.radius);++dy)
        for(uint c=0;c<4;++c)sums[c]+=blur_vertical_sample(src,p,int(t.y)+dy,t.x,c);
    for(uint c=0;c<4;++c)dst[p.d0+t.y*p.ds+t.x*4+c]=uchar((sums[c]*p.reciprocal)>>16);
}
struct Displace { uint width,height,ds,ss,as,bs,d0,s0,a0,b0,strength,filtered,second,sw,sh; };
int displace_word(device const uchar* map,uint at) {return int(as_type<short>(ushort(uint(map[at])|(uint(map[at+1])<<8))));}
int displace_wrap(int value) {return int(as_type<short>(ushort(value)));}
int displace_sample(device const uchar* src,constant Displace& p,int x,int y,uint c) {
    if(x<0 || y<0 || uint(x)>=p.sw || uint(y)>=p.sh)return 0;
    return int(src[p.s0+uint(y)*p.ss+uint(x)*4+c]);
}
kernel void effector_displacement(device uchar* dst [[buffer(0)]],device const uchar* src [[buffer(1)]],
    device const uchar* first [[buffer(2)]],device const uchar* second [[buffer(3)]],constant Displace& p [[buffer(4)]],
    uint2 t [[thread_position_in_grid]]) {
    if(t.x>=p.width || t.y>=p.height)return;
    uint out=p.d0+t.y*p.ds+t.x*4;
    if(p.second==0 && p.strength==0) {for(uint c=0;c<4;++c)dst[out+c]=src[p.s0+t.y*p.ss+t.x*4+c];return;}
    bool dual=p.second!=0 && p.strength>0 && p.strength<256;
    bool use_second=p.second!=0 && p.strength==256;
    uint effective=p.second!=0 && (p.strength==0 || p.strength==256)?256:p.strength;
    uint bits=p.filtered!=0?(dual || effective<256?3:4):0;
    int2 d;
    for(uint c=0;c<2;++c) {
        int a=use_second?displace_word(second,p.b0+t.y*p.bs+t.x*4+c*2):displace_word(first,p.a0+t.y*p.as+t.x*4+c*2);
        if(dual) {
            int difference=displace_wrap(displace_word(second,p.b0+t.y*p.bs+t.x*4+c*2)-a);
            d[c]=displace_wrap((a>>1)+((difference*int(p.strength<<7))>>16));
            if(p.filtered==0)d[c]=displace_wrap(d[c]+4)>>3;
        } else if(p.filtered!=0)d[c]=effective==256?a:(a*int(effective<<7))>>16;
        else d[c]=(a*int(effective)+8)>>12;
    }
    int sx=displace_wrap(int(t.x)+(d.x>>bits)),sy=displace_wrap(int(t.y)+(d.y>>bits));
    int fx=d.x&int((1u<<bits)-1),fy=d.y&int((1u<<bits)-1);
    for(uint c=0;c<4;++c) {
        int value=displace_sample(src,p,sx,sy,c);
        if(p.filtered!=0) {
            int top=value+(((displace_sample(src,p,sx+1,sy,c)-value)*fx)>>bits);
            int bottom=displace_sample(src,p,sx,sy+1,c);
            bottom+=((displace_sample(src,p,sx+1,sy+1,c)-bottom)*fx)>>bits;
            value=top+(((bottom-top)*fy)>>bits);
        }
        dst[out+c]=uchar(clamp(value,0,255));
    }
}
struct Vector { uint width,height,ds,ss,ms,d0,s0,m0,filtered,rectangular; };
int vector_word(device const uchar* map,uint at) {return int(as_type<short>(ushort(uint(map[at])|(uint(map[at+1])<<8))));}
int vector_wrap(int value) {return int(as_type<short>(ushort(value)));}
int vector_sample(device const uchar* src,constant Vector& p,int x,int y,uint c) {
    if(p.rectangular!=0) {
        if(x<0 || y<0 || uint(x)>=p.width || uint(y)>=p.height)return 0;
        return int(src[p.s0+uint(y)*p.ss+uint(x)*4+c]);
    }
    // Host admission bounds stride/4 so signed16 coordinates cannot overflow.
    int index=y*int(p.ss/4)+x;
    if(index<0 || uint(index)>=(p.ss/4)*p.height)return 0;
    return int(src[p.s0+uint(index)*4+c]);
}
kernel void effector_vector(device uchar* dst [[buffer(0)]],device const uchar* src [[buffer(1)]],
    device const uchar* map [[buffer(2)]],device const uint* coefficients [[buffer(3)]],constant Vector& p [[buffer(4)]],
    uint2 t [[thread_position_in_grid]]) {
    if(t.x>=p.width || t.y>=p.height)return;
    uint at=p.m0+t.y*p.ms+t.x*6;
    uint index=uint(map[at+4])|(uint(map[at+5])<<8),coefficient=coefficients[index];
    int qx=vector_word(map,at)*int(as_type<short>(ushort(coefficient)));
    int qy=vector_word(map,at+2)*int(as_type<short>(ushort(coefficient>>16)));
    int dx=(qx>>16)+(p.filtered==0 && (uint(qx)&65535u)>=32768u?1:0);
    int dy=(qy>>16)+(p.filtered==0 && (uint(qy)&65535u)>=32768u?1:0);
    int sx=vector_wrap(int(t.x)+dx),sy=vector_wrap(int(t.y)+dy);
    int fx=int((uint(qx)>>12)&15u),fy=int((uint(qy)>>12)&15u);
    for(uint c=0;c<4;++c) {
        int value=vector_sample(src,p,sx,sy,c);
        if(p.filtered!=0) {
            int top=value+(((vector_sample(src,p,sx+1,sy,c)-value)*fx)>>4);
            int bottom=vector_sample(src,p,sx,sy+1,c);
            bottom+=((vector_sample(src,p,sx+1,sy+1,c)-bottom)*fx)>>4;
            value=top+(((bottom-top)*fy)>>4);
        }
        dst[p.d0+t.y*p.ds+t.x*4+c]=uchar(clamp(value,0,255));
    }
}
struct TransformExact { uint dw,dh,ds,sw,sh,ss,d0,s0,sx,sy,cx,cy,rx,ry,filtered; };
int transform_exact_sample(device const uchar* src,constant TransformExact& p,int x,int y,uint c) {
    if(x<0 || y<0 || uint(x)>=p.sw || uint(y)>=p.sh)return 0;
    return int(src[p.s0+uint(y)*p.ss+uint(x)*4+c]);
}
kernel void effector_transform(device uchar* dst [[buffer(0)]],device const uchar* src [[buffer(1)]],
    constant TransformExact& p [[buffer(2)]],uint2 t [[thread_position_in_grid]]) {
    if(t.x>=p.dw || t.y>=p.dh)return;
    uint qx=p.sx+t.x*p.cx+t.y*p.rx,qy=p.sy+t.x*p.cy+t.y*p.ry;
    if(p.filtered==0){qx+=32768u;qy+=32768u;}
    int x=as_type<int>(qx)>>16,y=as_type<int>(qy)>>16;
    int fx=int((qx>>12)&15u),fy=int((qy>>12)&15u);
    for(uint c=0;c<4;++c) {
        int value=transform_exact_sample(src,p,x,y,c);
        if(p.filtered!=0) {
            int top=value+(((transform_exact_sample(src,p,x+1,y,c)-value)*fx)>>4);
            int bottom=transform_exact_sample(src,p,x,y+1,c);
            bottom+=((transform_exact_sample(src,p,x+1,y+1,c)-bottom)*fx)>>4;
            value=top+(((bottom-top)*fy)>>4);
        }
        dst[p.d0+t.y*p.ds+t.x*4+c]=uchar(clamp(value,0,255));
    }
}
struct Temporal { uint ss,hs,s0,h0,sw,sh,hw,hh,format,strength; };
#pragma clang fp contract(off)
kernel void effector_temporal(device uchar* scene [[buffer(0)]],device const uchar* history [[buffer(1)]],
    device const uint* rcp_table [[buffer(2)]],constant Temporal& p [[buffer(3)]],uint2 t [[thread_position_in_grid]]) {
    uint x=t.x*2u,y=t.y;
    if(x>=p.hw || y>=p.hh || p.strength==0 || p.strength>256)return;
    uint count=min(2u,p.hw-x),h=p.h0+y*p.hs+x*4,s=p.s0+y*p.ss+x*4;
    if(p.format==2 && history[h+3]==0 && (count==1 || history[h+7]==0))return;
    for(uint i=0;i<count;++i) {
        uint hi=h+i*4,si=s+i*4;
        if(p.strength==256 && (p.format==1 || (count==1 && history[hi+3]==255) ||
                (count==2 && history[h+3]==255 && history[h+7]==255))) {
            for(uint c=0;c<4;++c)scene[si+c]=history[hi+c];
        } else if(p.format==1) {
            for(uint c=0;c<4;++c)scene[si+c]=uchar(int(history[hi+c])+(((int(scene[si+c])-int(history[hi+c]))*int((256-p.strength)>>1))>>7));
        } else {
            uint ha=history[hi+3],sa=scene[si+3],hw,sw,alpha;
            if(count==2) {
                float first=float(ha)*(float(p.strength)/256.0f);
                float second=(float(sa)/256.0f)*(256.0f-first);
                float total=first+second;
                uint bits=as_type<uint>(total==0.0f?1.0f:total);
                float reciprocal=as_type<float>(rcp_table[(bits>>11)&4095u]-((((bits>>23)&255u)-127u)<<23));
                float a=reciprocal*first,b=reciprocal*second;
                hw=uint(a*256.0f);sw=uint(b*256.0f);alpha=uint(total);
            } else {
                uint first=ha*p.strength,second=(sa*(65536u-first))>>8,total=first+second;
                if(total==0){hw=0;sw=0;alpha=0;}else{hw=(first<<8)/total;sw=(second<<8)/total;alpha=total>>8;}
            }
            for(uint c=0;c<3;++c)scene[si+c]=uchar(((uint(history[hi+c])*hw+uint(scene[si+c])*sw)&65535u)>>8);
            scene[si+3]=uchar(alpha);
        }
    }
}
kernel void effector_temporal_retain(device const uchar* scene [[buffer(0)]],device uchar* history [[buffer(1)]],
    constant Temporal& p [[buffer(2)]],uint2 t [[thread_position_in_grid]]) {
    if(t.x>=p.sw || t.y>=p.sh)return;
    for(uint c=0;c<4;++c)history[p.h0+t.y*p.hs+t.x*4+c]=scene[p.s0+t.y*p.ss+t.x*4+c];
}
struct Parameters { uint dw,dh,ds,sw,sh,ss,start_x,start_y,dx,dy,parameter,filtered; };
int floor_shift(int v,uint bits) {
    return v>=0 ? int(uint(v)>>bits) : -int((uint(-(v+1))+1u+((1u<<bits)-1u))>>bits);
}
int sample(device const uchar* src,constant Parameters& p,int x,int y,uint c) {
    if(x<0 || y<0 || uint(x)>=p.sw || uint(y)>=p.sh)return 0;
    return int(src[uint(y)*p.ss+uint(x)*4u+c]);
}
kernel void scaled_alpha(device uchar* dst [[buffer(0)]], device const uchar* src [[buffer(1)]],
                         constant Parameters& p [[buffer(2)]], uint2 tid [[thread_position_in_grid]]) {
    if(tid.x>=p.dw || tid.y>=p.dh)return;
    uint qx=p.start_x+tid.x*p.dx, qy=p.start_y+tid.y*p.dy;
    int ix=floor_shift(as_type<int>(qx),16), iy=floor_shift(as_type<int>(qy),16);
    int4 color=int4(0);
    for(uint c=0;c<4;++c) {
        if(p.filtered) {
            int fx=int((qx&65535u)>>12),fy=int((qy&65535u)>>12);
            int tl=sample(src,p,ix,iy,c),bl=sample(src,p,ix,iy+1,c);
            int top=tl+floor_shift((sample(src,p,ix+1,iy,c)-tl)*fx,4);
            int bottom=bl+floor_shift((sample(src,p,ix+1,iy+1,c)-bl)*fx,4);
            color[c]=top+floor_shift((bottom-top)*fy,4);
        } else color[c]=sample(src,p,ix,iy,c);
    }
    uint index=uint(color.w)>>1;
    int weight=int(((index==127u?128u:index)*(256u-p.parameter))>>8);
    uint at=tid.y*p.ds+tid.x*4u;
    for(uint c=0;c<3;++c) {
        int old=int(dst[at+c]);
        dst[at+c]=uchar(old+floor_shift((color[c]-old)*weight,7));
    }
}

struct Crossfade {uint width,height,ds,as,bs,d0,a0,b0,mix,opacity;};
kernel void crossfade(device uchar* dst [[buffer(0)]],device const uchar* a [[buffer(1)]],
                      device const uchar* b [[buffer(2)]],constant Crossfade& p [[buffer(3)]],
                      uint2 tid [[thread_position_in_grid]]) {
    uint x=tid.x*2u,y=tid.y;
    if(x>=p.width || y>=p.height)return;
    uint count=min(2u,p.width-x);
    int4 color[2]={int4(0),int4(0)};
    for(uint i=0;i<count;++i) {
        uint ai=p.a0+y*p.as+(x+i)*4u,bi=p.b0+y*p.bs+(x+i)*4u;
        int aa=int((uint(a[ai+3u])*(256u-p.opacity))>>8);
        int ab=int((uint(b[bi+3u])*(256u-p.opacity))>>8);
        for(uint c=0;c<4;++c) {
            int ca=c==3u?aa:(int(a[ai+c])*aa)>>8;
            int cb=c==3u?ab:(int(b[bi+c])*ab)>>8;
            color[i][c]=ca+floor_shift((cb-ca)*int(p.mix),8);
        }
    }
    if(color[0].w==0 && (count==1u || color[1].w==0))return;
    for(uint i=0;i<count;++i) {
        uint out=p.d0+y*p.ds+(x+i)*4u;
        for(uint c=0;c<3;++c)
            dst[out+c]=uchar(clamp(color[i][c]+((int(dst[out+c])*(256-color[i].w))>>8),0,255));
        dst[out+3u]=0;
    }
}

// User-selected fast presentation: integer geometry, floating sample/blend.
struct AffineParameters {
    uint dw,dh,ds,sw,sh,ss,sx,sy,cx,cy,rx,ry,parameter,filtered,blend;
};
float4 affine_sample(device const uchar* src,constant AffineParameters& p,int x,int y) {
    if(x<0 || y<0 || uint(x)>=p.sw || uint(y)>=p.sh)return float4(0);
    uint at=uint(y)*p.ss+uint(x)*4u;
    return float4(src[at],src[at+1],src[at+2],src[at+3]);
}
kernel void affine_fast(device uchar* dst [[buffer(0)]],device const uchar* src [[buffer(1)]],
    constant AffineParameters& p [[buffer(2)]],uint2 tid [[thread_position_in_grid]]) {
    if(tid.x>=p.dw || tid.y>=p.dh)return;
    uint qx=p.sx+tid.x*p.cx+tid.y*p.rx, qy=p.sy+tid.x*p.cy+tid.y*p.ry;
    if(!p.filtered){qx+=32768u;qy+=32768u;}
    int x=int(qx)>>16,y=int(qy)>>16;
    float4 c=affine_sample(src,p,x,y);
    if(p.filtered) {
        float fx=float((qx>>12)&15u)/16.0f,fy=float((qy>>12)&15u)/16.0f;
        c=mix(mix(c,affine_sample(src,p,x+1,y),fx),
              mix(affine_sample(src,p,x,y+1),affine_sample(src,p,x+1,y+1),fx),fy);
    }
    uint at=tid.y*p.ds+tid.x*4u;
    if(p.blend) {
        float alpha=c.w*float(256u-p.parameter)/(255.0f*256.0f);
        for(uint ch=0;ch<3u;++ch)
            dst[at+ch]=uchar(clamp(floor(mix(float(dst[at+ch]),c[ch],alpha)+0.5f),0.0f,255.0f));
    } else {
        for(uint ch=0;ch<4u;++ch)dst[at+ch]=uchar(clamp(floor(c[ch]+0.5f),0.0f,255.0f));
    }
}
struct MaskParameters {
    uint ds,ss,ms,doff,soff,moff,left,top,width,height,sx,sy,mx,my,shift;
};
kernel void background_mask(device uchar* dst [[buffer(0)]],
    device const uchar* src [[buffer(1)]],device const uchar* mask [[buffer(2)]],
    constant MaskParameters& p [[buffer(3)]],constant int* factors [[buffer(4)]],
    uint2 tid [[thread_position_in_grid]]) {
    if(tid.x>=p.width || tid.y>=p.height)return;
    uint mi=p.moff+(p.my+tid.y)*p.ms+p.mx+tid.x;
    int factor=factors[mask[mi]];
    if(factor==0)return;
    uint si=p.soff+(p.sy+tid.y)*p.ss+(p.sx+tid.x)*4u;
    uint di=p.doff+(p.top+tid.y)*p.ds+(p.left+tid.x)*4u;
    if(factor<0){for(uint c=0;c<4;++c)dst[di+c]=src[si+c];return;}
    for(uint c=0;c<3;++c) {
        int old=dst[di+c],delta=int(src[si+c])-old;
        dst[di+c]=uchar(clamp(old+floor_shift(delta*factor,p.shift),0,255));
    }
}

struct Transfer { uint ds,ss,d0,s0,width,height,df,sf,op,parameter,color; };
kernel void pixel_transfer(device uchar* dst [[buffer(0)]],device const uchar* src [[buffer(1)]],
    constant Transfer& p [[buffer(2)]],uint2 t [[thread_position_in_grid]]) {
    if(t.x>=p.width || t.y>=p.height)return;
    uint d=p.d0+t.y*p.ds+t.x*4u,s=p.s0+t.y*p.ss+t.x*4u;
    if(p.op==5u){for(uint c=0;c<4u;++c)dst[d+c]=0;return;}
    if(p.op==3u) {
        for(uint c=0;c<3u;++c)
            dst[d+c]=uchar((uint(dst[d+c])*(256u-p.parameter)+((p.color>>(c*8u))&255u)*p.parameter)>>8u);
        if(p.color==0u && p.parameter!=0u)dst[d+3u]=0;
        return;
    }
    if(p.op==0u) {
        for(uint c=0;c<4u;++c)dst[d+c]=src[s+c];
        if(p.df==2u && p.sf==1u)dst[d+3u]=255;
        if(p.df==1u && p.sf==2u) {
            uint a=uint(src[s+3u])>>1u;if(a==127u)a=128u;
            for(uint c=0;c<3u;++c)dst[d+c]=uchar((uint(src[s+c])*a)>>7u);
            dst[d+3u]=0;
        }
        return;
    }
    for(uint c=0;c<4u;++c) {
        int a=int(src[s+c]),b=int(dst[d+c]);
        if(p.op==1u)dst[d+c]=uchar((uint(a)*(256u-p.parameter))>>8u);
        else if(p.op==2u)dst[d+c]=uchar(a+floor_shift((b-a)*int(p.parameter>>1u),7u));
        else {
            uint factor=((uint(src[s+3u])>>1u)*(256u-p.parameter))>>8u;
            dst[d+c]=uchar(b+floor_shift((a-b)*int(factor),7u));
        }
    }
}
struct Layer { uint ds,ss,d0,s0,width,height,format; };
kernel void layer_composite(device uchar* dst [[buffer(0)]], device const uchar* src [[buffer(1)]],
    device const uchar* corrections [[buffer(2)]], constant Layer& p [[buffer(3)]],
    uint2 tid [[thread_position_in_grid]]) {
    uint x=tid.x*2u,y=tid.y;
    if(x>=p.width || y>=p.height)return;
    uint count=min(2u,p.width-x),si=p.s0+y*p.ss+x*4u,di=p.d0+y*p.ds+x*4u;
    uint a=src[si+3u],b=count==2u?uint(src[si+7u]):0u;
    if(count==2u && a==0u && b==0u)return;
    if(count==2u && (p.format==1u?(a>=254u && b>=254u):(a==255u && b==255u))) {
        for(uint c=0;c<8u;++c)dst[di+c]=src[si+c];return;
    }
    for(uint i=0;i<count;++i) {
        uint source=si+i*4u,out=di+i*4u,alpha=src[source+3u];
        if(p.format==1u) {
            uint factor=alpha>>1u;
            if(factor==127u) {
                for(uint c=0;c<3u;++c)dst[out+c]=src[source+c];
                if(count==1u)dst[out+3u]=0;
            } else if(factor!=0u) {
                for(uint c=0;c<3u;++c) {
                    int old=dst[out+c];dst[out+c]=uchar(old+floor_shift((int(src[source+c])-old)*int(factor),7));
                }
            }
        } else {
            if(count==1u && alpha==0u)continue;
            if(count==1u && alpha==255u) {
                for(uint c=0;c<4u;++c)dst[out+c]=src[source+c];continue;
            }
            uint da=dst[out+3u],dc=da*(256u-alpha),total=alpha*256u+dc;
            int sw=total==0u?0:int((alpha<<16u)/total);
            int dw=total==0u?0:int((dc<<8u)/total);
            if(count==2u) {
                uint fix=corrections[alpha*256u+da];
                if(fix==1u)--sw;else if(fix==2u)++sw;else if(fix==3u)--dw;else if(fix==4u)++dw;
            }
            for(uint c=0;c<3u;++c)
                dst[out+c]=uchar((uint(int(src[source+c])*sw+int(dst[out+c])*dw)&65535u)>>8u);
            dst[out+3u]=uchar(total>>8u);
        }
    }
}
)MSL";
}
namespace {
class MetalPixelStorage final : public GpuPixelStorage {
public:
    id<MTLDevice> device;id<MTLCommandQueue> queue;id<MTLBuffer> buffer;
    MetalPixelStorage(id<MTLDevice> d,id<MTLCommandQueue> q,id<MTLBuffer> b):
        device([d retain]),queue([q retain]),buffer([b retain]) {}
    ~MetalPixelStorage() override {[buffer release];[queue release];[device release];}
    size_t size() const override {return [buffer length];}
    bool fast_ranges() const override {return [buffer storageMode]==MTLStorageModeShared;}
    bool read_range(size_t offset,std::span<uint8_t> out) const override {
        if(offset>size() || out.size()>size()-offset)return false;
        if(!fast_ranges())return GpuPixelStorage::read_range(offset,out);
        std::memcpy(out.data(),static_cast<const uint8_t*>([buffer contents])+offset,out.size());return true;
    }
    bool read(std::span<uint8_t> destination) const override {
        if(destination.size()!=size())return false;
        // Stored bitmap allocations are immutable shader inputs. On unified
        // memory the shared MTLBuffer is the single GPU allocation, so a
        // native read needs no staging allocation, GPU copy or queue wait.
        if([buffer storageMode]==MTLStorageModeShared) {
            std::memcpy(destination.data(),[buffer contents],destination.size());
            return true;
        }
        @autoreleasepool {
            id<MTLBuffer> staging=[device newBufferWithLength:size() options:MTLResourceStorageModeShared];
            if(!staging)return false;
            id<MTLCommandBuffer> command=[queue commandBuffer];
            id<MTLBlitCommandEncoder> encoder=[command blitCommandEncoder];
            if(!command || !encoder){[staging release];return false;}
            [encoder copyFromBuffer:buffer sourceOffset:0 toBuffer:staging destinationOffset:0 size:size()];
            [encoder endEncoding];[command commit];[command waitUntilCompleted];
            const bool success=[command status]==MTLCommandBufferStatusCompleted;
            if(success)std::memcpy(destination.data(),[staging contents],destination.size());
            [staging release];return success;
        }
    }
};
}
namespace {
std::shared_ptr<MetalPixelStorage> metal_storage(const std::shared_ptr<GpuPixelStorage>& value) {
    if(auto stored=std::dynamic_pointer_cast<MetalPixelStorage>(value))return stored;
    return value?std::dynamic_pointer_cast<MetalPixelStorage>(value->concrete_storage()):nullptr;
}
}
struct GpuPixelKernels::Impl {
    id<MTLDevice> device=nil;
    id<MTLCommandQueue> queue=nil;
    id<MTLComputePipelineState> pipeline=nil,crossfade_pipeline=nil,affine_pipeline=nil,mask_pipeline=nil,layer_pipeline=nil,transfer_pipeline=nil;
    id<MTLComputePipelineState> blur_horizontal=nil,blur_vertical=nil,blur_horizontal_pixels=nil,blur_vertical_pixels=nil,displacement_pipeline=nil,vector_pipeline=nil,transform_pipeline=nil,temporal_pipeline=nil,temporal_retain_pipeline=nil;
    id<MTLBuffer> destination=nil,source=nil,second=nil,auxiliary=nil;
    ~Impl() {
        [blur_horizontal release];[blur_vertical release];[displacement_pipeline release];[vector_pipeline release];[transform_pipeline release];[temporal_pipeline release];[temporal_retain_pipeline release];[auxiliary release];
        [blur_horizontal_pixels release];[blur_vertical_pixels release];
        [source release];[second release];[destination release];[pipeline release];[crossfade_pipeline release];[affine_pipeline release];[mask_pipeline release];[layer_pipeline release];[transfer_pipeline release];[queue release];[device release];
    }
};
GpuPixelKernels::GpuPixelKernels(bool enabled) : impl_(std::make_unique<Impl>()) {
    if(!enabled)return;
    @autoreleasepool {
        impl_->device=MTLCreateSystemDefaultDevice();
        if(!impl_->device)return;
        NSError* error=nil;
        MTLCompileOptions* compile_options=[MTLCompileOptions new];compile_options.fastMathEnabled=NO;
        id<MTLLibrary> library=[impl_->device newLibraryWithSource:[NSString stringWithUTF8String:shader_source] options:compile_options error:&error];
        [compile_options release];
        if(!library) {
            std::cerr<<"warning: METAL_SHADER_COMPILE_FAILED "<<[[error localizedDescription] UTF8String]<<'\n';
            return;
        }
        id<MTLFunction> function=[library newFunctionWithName:@"scaled_alpha"];
        impl_->pipeline=[impl_->device newComputePipelineStateWithFunction:function error:&error];
        [function release];
        function=[library newFunctionWithName:@"crossfade"];
        impl_->crossfade_pipeline=[impl_->device newComputePipelineStateWithFunction:function error:&error];
        [function release];
        function=[library newFunctionWithName:@"affine_fast"];
        impl_->affine_pipeline=[impl_->device newComputePipelineStateWithFunction:function error:&error];
        [function release];
        function=[library newFunctionWithName:@"background_mask"];
        impl_->mask_pipeline=[impl_->device newComputePipelineStateWithFunction:function error:&error];
        [function release];
        function=[library newFunctionWithName:@"layer_composite"];
        impl_->layer_pipeline=[impl_->device newComputePipelineStateWithFunction:function error:&error];
        [function release];
        function=[library newFunctionWithName:@"pixel_transfer"];
        impl_->transfer_pipeline=[impl_->device newComputePipelineStateWithFunction:function error:&error];
        [function release];
        function=[library newFunctionWithName:@"effector_horizontal"];
        impl_->blur_horizontal=[impl_->device newComputePipelineStateWithFunction:function error:&error];
        [function release];
        function=[library newFunctionWithName:@"effector_vertical"];
        impl_->blur_vertical=[impl_->device newComputePipelineStateWithFunction:function error:&error];
        [function release];
        function=[library newFunctionWithName:@"effector_horizontal_pixels"];
        impl_->blur_horizontal_pixels=[impl_->device newComputePipelineStateWithFunction:function error:&error];
        [function release];
        function=[library newFunctionWithName:@"effector_vertical_pixels"];
        impl_->blur_vertical_pixels=[impl_->device newComputePipelineStateWithFunction:function error:&error];
        [function release];
        function=[library newFunctionWithName:@"effector_displacement"];
        impl_->displacement_pipeline=[impl_->device newComputePipelineStateWithFunction:function error:&error];
        [function release];
        function=[library newFunctionWithName:@"effector_vector"];
        impl_->vector_pipeline=[impl_->device newComputePipelineStateWithFunction:function error:&error];
        [function release];
        function=[library newFunctionWithName:@"effector_transform"];
        impl_->transform_pipeline=[impl_->device newComputePipelineStateWithFunction:function error:&error];[function release];
        function=[library newFunctionWithName:@"effector_temporal"];
        impl_->temporal_pipeline=[impl_->device newComputePipelineStateWithFunction:function error:&error];[function release];
        function=[library newFunctionWithName:@"effector_temporal_retain"];
        impl_->temporal_retain_pipeline=[impl_->device newComputePipelineStateWithFunction:function error:&error];
        [function release];[library release];
        if(impl_->pipeline && impl_->crossfade_pipeline)impl_->queue=[impl_->device newCommandQueue];
    }
}
GpuPixelKernels::~GpuPixelKernels()=default;
bool GpuPixelKernels::available() const { return impl_->queue && impl_->pipeline; }
std::string GpuPixelKernels::device_name() const {
    return available()?std::string([[impl_->device name] UTF8String]):"CPU";
}
PixelKernelResult GpuPixelKernels::scaled_alpha(const ScaledAlphaPixels& p) {
    const bool timing=debug_enabled();
    const auto start=timing?Clock::now():Clock::time_point{};
    PixelKernelResult result;
    const auto fallback=[&]() {
        result.success=compose_scaled_alpha_bgra(p.destination,p.destination_width,p.destination_height,
            p.destination_stride,p.source,p.source_width,p.source_height,p.source_stride,p.x_q16,p.y_q16,
            p.origin_x_q16,p.origin_y_q16,p.scale_x_q16,p.scale_y_q16,p.parameter,p.filtered);
        if(timing)result.total_ns=elapsed(start);
        return result;
    };
    if(!available() || !supported(p) || p.destination_width==0 || p.destination_height==0 || p.source.empty())return fallback();
    @autoreleasepool {
        const auto upload=timing?Clock::now():Clock::time_point{};
        const auto ensure=[&](id<MTLBuffer>& buffer,size_t size) {
            if(buffer && [buffer length]>=size)return true;
            [buffer release];buffer=[impl_->device newBufferWithLength:size options:MTLResourceStorageModeShared];
            return buffer!=nil;
        };
        if(!ensure(impl_->source,p.source.size()) || !ensure(impl_->destination,p.destination.size()))return fallback();
        std::memcpy([impl_->source contents],p.source.data(),p.source.size());
        std::memcpy([impl_->destination contents],p.destination.data(),p.destination.size());
        if(timing)result.upload_ns=elapsed(upload);
        const auto wait=timing?Clock::now():Clock::time_point{};
        id<MTLCommandBuffer> command=[impl_->queue commandBuffer];
        id<MTLComputeCommandEncoder> encoder=[command computeCommandEncoder];
        if(!command || !encoder)return fallback();
        const auto args=parameters(p);
        [encoder setComputePipelineState:impl_->pipeline];
        [encoder setBuffer:impl_->destination offset:0 atIndex:0];
        [encoder setBuffer:impl_->source offset:0 atIndex:1];
        [encoder setBytes:&args length:sizeof(args) atIndex:2];
        const NSUInteger side=16;
        [encoder dispatchThreads:MTLSizeMake(p.destination_width,p.destination_height,1)
            threadsPerThreadgroup:MTLSizeMake(side,side,1)];
        [encoder endEncoding];[command commit];[command waitUntilCompleted];
        if(timing)result.wait_ns=elapsed(wait);
        if([command status]!=MTLCommandBufferStatusCompleted)return fallback();
        const auto download=timing?Clock::now():Clock::time_point{};
        std::memcpy(p.destination.data(),[impl_->destination contents],p.destination.size());
        if(timing)result.download_ns=elapsed(download);
        result.success=true;result.used_gpu=true;if(timing)result.total_ns=elapsed(start);
        return result;
    }
}
PixelKernelResult GpuPixelKernels::crossfade(const CrossfadePixels& p) {
    const bool timing=debug_enabled();
    const auto start=timing?Clock::now():Clock::time_point{};
    PixelKernelResult result;
    const auto fallback=[&]() {
        result.success=blend_original_sprite_crossfade(p.destination,p.first,p.second,p.mix,p.opacity);
        if(timing)result.total_ns=elapsed(start);
        return result;
    };
    const uint32_t width=std::min(p.first.width,p.second.width),height=std::min(p.first.height,p.second.height);
    if(!available() || p.destination.format!=1 || p.first.format!=2 || p.second.format!=2 ||
       p.mix<0 || p.mix>256 || p.opacity>=256 || !width || !height ||
       p.destination.width<width || p.destination.height<height)return fallback();
    const auto valid=[&](const GlyphBitmapView& v) {
        return v.bytes_per_pixel==4 && v.stride>=uint64_t(width)*4 && v.byte_offset<=v.pixels.size() &&
            uint64_t(height-1)*v.stride+uint64_t(width)*4<=v.pixels.size()-v.byte_offset &&
            v.pixels.size()<=UINT32_MAX;
    };
    const auto overlaps=[](std::span<uint8_t> a,std::span<uint8_t> b) {
        uintptr_t x=reinterpret_cast<uintptr_t>(a.data()),y=reinterpret_cast<uintptr_t>(b.data());
        return x<=y?y-x<a.size():x-y<b.size();
    };
    if(!valid(p.destination) || !valid(p.first) || !valid(p.second) ||
       overlaps(p.destination.pixels,p.first.pixels) || overlaps(p.destination.pixels,p.second.pixels))return fallback();
    @autoreleasepool {
        const auto upload=timing?Clock::now():Clock::time_point{};
        const auto ensure=[&](id<MTLBuffer>& buffer,size_t size) {
            if(buffer && [buffer length]>=size)return true;
            [buffer release];buffer=[impl_->device newBufferWithLength:size options:MTLResourceStorageModeShared];
            return buffer!=nil;
        };
        if(!ensure(impl_->source,p.first.pixels.size()) || !ensure(impl_->second,p.second.pixels.size()) ||
           !ensure(impl_->destination,p.destination.pixels.size()))return fallback();
        std::memcpy([impl_->source contents],p.first.pixels.data(),p.first.pixels.size());
        std::memcpy([impl_->second contents],p.second.pixels.data(),p.second.pixels.size());
        std::memcpy([impl_->destination contents],p.destination.pixels.data(),p.destination.pixels.size());
        if(timing)result.upload_ns=elapsed(upload);
        const auto wait=timing?Clock::now():Clock::time_point{};
        id<MTLCommandBuffer> command=[impl_->queue commandBuffer];
        id<MTLComputeCommandEncoder> encoder=[command computeCommandEncoder];
        if(!command || !encoder)return fallback();
        const uint32_t args[]={width,height,p.destination.stride,p.first.stride,p.second.stride,
            p.destination.byte_offset,p.first.byte_offset,p.second.byte_offset,uint32_t(p.mix),p.opacity};
        [encoder setComputePipelineState:impl_->crossfade_pipeline];
        [encoder setBuffer:impl_->destination offset:0 atIndex:0];
        [encoder setBuffer:impl_->source offset:0 atIndex:1];
        [encoder setBuffer:impl_->second offset:0 atIndex:2];
        [encoder setBytes:args length:sizeof(args) atIndex:3];
        [encoder dispatchThreads:MTLSizeMake((width+1U)/2U,height,1) threadsPerThreadgroup:MTLSizeMake(16,16,1)];
        [encoder endEncoding];[command commit];[command waitUntilCompleted];
        if(timing)result.wait_ns=elapsed(wait);
        if([command status]!=MTLCommandBufferStatusCompleted)return fallback();
        const auto download=timing?Clock::now():Clock::time_point{};
        std::memcpy(p.destination.pixels.data(),[impl_->destination contents],p.destination.pixels.size());
        if(timing)result.download_ns=elapsed(download);
        result.success=true;result.used_gpu=true;if(timing)result.total_ns=elapsed(start);
        return result;
    }
}

PixelKernelResult GpuPixelKernels::affine(const AffinePixels& p) {
    PixelKernelResult result;
    const auto resident=metal_storage(p.source_storage);
    if(p.source_storage && (!resident || resident->device!=impl_->device || !p.source.empty()))return result;
    const bool timing=debug_enabled();
    const auto start=timing?Clock::now():Clock::time_point{};
    const auto valid=[](size_t size,uint32_t width,uint32_t height,uint32_t stride) {
        return width && height && width<=32767 && height<=32767 && stride%4U==0 &&
            stride>=uint64_t(width)*4U && size<=UINT32_MAX && size>=uint64_t(stride)*height;
    };
    const uintptr_t d=reinterpret_cast<uintptr_t>(p.destination.data()),
                    a=reinterpret_cast<uintptr_t>(p.source.data());
    const bool overlap=d<=a?a-d<p.destination.size():d-a<p.source.size();
    if(!available() || !impl_->affine_pipeline || p.parameter>256 || (!resident && overlap) ||
       !valid(p.destination.size(),p.destination_width,p.destination_height,p.destination_stride) ||
       !valid(resident?resident->size():p.source.size(),p.source_width,p.source_height,p.source_stride))return result;
    @autoreleasepool {
        const auto upload=timing?Clock::now():Clock::time_point{};
        const auto ensure=[&](id<MTLBuffer>& buffer,size_t size) {
            if(buffer && [buffer length]>=size)return true;
            [buffer release];buffer=[impl_->device newBufferWithLength:size options:MTLResourceStorageModeShared];
            return buffer!=nil;
        };
        if((!resident && !ensure(impl_->source,p.source.size())) || !ensure(impl_->destination,p.destination.size()))return result;
        if(!resident)std::memcpy([impl_->source contents],p.source.data(),p.source.size());
        std::memcpy([impl_->destination contents],p.destination.data(),p.destination.size());
        if(timing)result.upload_ns=elapsed(upload);
        const auto wait=timing?Clock::now():Clock::time_point{};
        id<MTLCommandBuffer> command=[impl_->queue commandBuffer];
        id<MTLComputeCommandEncoder> encoder=[command computeCommandEncoder];
        if(!command || !encoder)return result;
        const uint32_t args[]={p.destination_width,p.destination_height,p.destination_stride,
            p.source_width,p.source_height,p.source_stride,uint32_t(p.matrix[0]),uint32_t(p.matrix[1]),
            uint32_t(p.matrix[2]),uint32_t(p.matrix[3]),uint32_t(p.matrix[4]),uint32_t(p.matrix[5]),
            p.parameter,uint32_t(p.filtered),uint32_t(p.blend_alpha)};
        [encoder setComputePipelineState:impl_->affine_pipeline];
        [encoder setBuffer:impl_->destination offset:0 atIndex:0];
        [encoder setBuffer:resident?resident->buffer:impl_->source offset:0 atIndex:1];
        [encoder setBytes:args length:sizeof(args) atIndex:2];
        [encoder dispatchThreads:MTLSizeMake(p.destination_width,p.destination_height,1)
            threadsPerThreadgroup:MTLSizeMake(16,16,1)];
        [encoder endEncoding];[command commit];[command waitUntilCompleted];
        if(timing)result.wait_ns=elapsed(wait);
        if([command status]!=MTLCommandBufferStatusCompleted)return result;
        const auto download=timing?Clock::now():Clock::time_point{};
        std::memcpy(p.destination.data(),[impl_->destination contents],p.destination.size());
        if(timing){result.download_ns=elapsed(download);result.total_ns=elapsed(start);}
        result.success=true;result.used_gpu=true;result.approximate=true;
        return result;
    }
}

PixelKernelResult GpuPixelKernels::background_mask(const BackgroundMaskPixels& p) {
    PixelKernelResult result;
    const bool timing=debug_enabled();const auto start=timing?Clock::now():Clock::time_point{};
    const auto valid=[](const GlyphBitmapView& v,uint32_t format,uint32_t bpp) {
        return v.format==format && v.bytes_per_pixel==bpp && v.width && v.height &&
            v.pixels.size()<=UINT32_MAX && v.stride>=uint64_t(v.width)*bpp &&
            uint64_t(v.byte_offset)+uint64_t(v.height-1)*v.stride+uint64_t(v.width)*bpp<=v.pixels.size();
    };
    const auto overlaps=[](std::span<uint8_t> a,std::span<uint8_t> b) {
        const uintptr_t x=reinterpret_cast<uintptr_t>(a.data()),y=reinterpret_cast<uintptr_t>(b.data());
        return x<=y?y-x<a.size():x-y<b.size();
    };
    if(!available() || !impl_->mask_pipeline || !valid(p.destination,1,4) ||
        !valid(p.source,1,4) || !valid(p.mask,3,1) || p.left>=p.right || p.top>=p.bottom ||
        p.right>p.destination.width || p.bottom>p.destination.height ||
        uint64_t(p.source_left)+p.right-p.left>p.source.width ||
        uint64_t(p.source_top)+p.bottom-p.top>p.source.height ||
        uint64_t(p.mask_left)+p.right-p.left>p.mask.width ||
        uint64_t(p.mask_top)+p.bottom-p.top>p.mask.height || (p.shift!=7 && p.shift!=12) ||
        overlaps(p.destination.pixels,p.source.pixels) || overlaps(p.destination.pixels,p.mask.pixels))return result;
    for(auto factor:p.factors)if(factor < -1 || factor>32767)return result;
    @autoreleasepool {
        const auto upload=timing?Clock::now():Clock::time_point{};
        const auto ensure=[&](id<MTLBuffer>& b,size_t size) {
            if(b && [b length]>=size)return true;
            [b release];b=[impl_->device newBufferWithLength:size options:MTLResourceStorageModeShared];return b!=nil;
        };
        if(!ensure(impl_->destination,p.destination.pixels.size()) ||
            !ensure(impl_->source,p.source.pixels.size()) || !ensure(impl_->second,p.mask.pixels.size()))return result;
        std::memcpy([impl_->destination contents],p.destination.pixels.data(),p.destination.pixels.size());
        std::memcpy([impl_->source contents],p.source.pixels.data(),p.source.pixels.size());
        std::memcpy([impl_->second contents],p.mask.pixels.data(),p.mask.pixels.size());
        if(timing)result.upload_ns=elapsed(upload);
        const auto wait=timing?Clock::now():Clock::time_point{};
        id<MTLCommandBuffer> command=[impl_->queue commandBuffer];
        id<MTLComputeCommandEncoder> encoder=[command computeCommandEncoder];
        if(!command || !encoder)return result;
        const uint32_t args[]={p.destination.stride,p.source.stride,p.mask.stride,
            p.destination.byte_offset,p.source.byte_offset,p.mask.byte_offset,p.left,p.top,
            p.right-p.left,p.bottom-p.top,p.source_left,p.source_top,p.mask_left,p.mask_top,p.shift};
        [encoder setComputePipelineState:impl_->mask_pipeline];
        [encoder setBuffer:impl_->destination offset:0 atIndex:0];
        [encoder setBuffer:impl_->source offset:0 atIndex:1];
        [encoder setBuffer:impl_->second offset:0 atIndex:2];
        [encoder setBytes:args length:sizeof(args) atIndex:3];
        [encoder setBytes:p.factors.data() length:sizeof(p.factors) atIndex:4];
        [encoder dispatchThreads:MTLSizeMake(p.right-p.left,p.bottom-p.top,1)
            threadsPerThreadgroup:MTLSizeMake(16,16,1)];
        [encoder endEncoding];[command commit];[command waitUntilCompleted];
        if(timing)result.wait_ns=elapsed(wait);
        if([command status]!=MTLCommandBufferStatusCompleted)return result;
        const auto download=timing?Clock::now():Clock::time_point{};
        std::memcpy(p.destination.pixels.data(),[impl_->destination contents],p.destination.pixels.size());
        if(timing){result.download_ns=elapsed(download);result.total_ns=elapsed(start);}
        result.success=true;result.used_gpu=true;return result;
    }
}

PixelKernelResult GpuPixelKernels::layer_composite(const LayerCompositePixels& p) {
    PixelKernelResult result;
    const bool timing=debug_enabled();const auto start=timing?Clock::now():Clock::time_point{};
    const auto valid=[](const GlyphBitmapView& v,uint32_t format,uint32_t bpp) {
        return v.format==format && v.bytes_per_pixel==bpp && v.width && v.height &&
            v.pixels.size()<=UINT32_MAX && v.stride>=uint64_t(v.width)*bpp &&
            uint64_t(v.byte_offset)+uint64_t(v.height-1)*v.stride+uint64_t(v.width)*bpp<=v.pixels.size();
    };
    const auto overlaps=[](std::span<uint8_t> a,std::span<uint8_t> b) {
        const uintptr_t x=reinterpret_cast<uintptr_t>(a.data()),y=reinterpret_cast<uintptr_t>(b.data());
        return x<=y?y-x<a.size():x-y<b.size();
    };
    const auto resident=metal_storage(p.source_storage);
    auto source_view=p.source;
    const auto source_size=resident?resident->size():source_view.pixels.size();
    const bool source_valid=source_view.format==2 && source_view.bytes_per_pixel==4 && source_view.width && source_view.height &&
        source_size<=UINT32_MAX && source_view.stride>=uint64_t(source_view.width)*4 &&
        uint64_t(source_view.byte_offset)+uint64_t(source_view.height-1)*source_view.stride+uint64_t(source_view.width)*4<=source_size;
    if((p.source_storage && (!resident || resident->device!=impl_->device || !p.source.pixels.empty())) ||
        !available() || !impl_->layer_pipeline ||
        !valid(p.destination,p.destination.format,4) || !source_valid ||
        (p.destination.format!=1 && p.destination.format!=2) ||
        p.destination.width!=p.source.width || p.destination.height!=p.source.height ||
        p.reciprocal_corrections.size()!=65536 ||
        (!resident && overlaps(p.destination.pixels,p.source.pixels)))return result;
    @autoreleasepool {
        const auto upload=timing?Clock::now():Clock::time_point{};
        const auto ensure=[&](id<MTLBuffer>& b,size_t size) {
            if(b && [b length]>=size)return true;
            [b release];b=[impl_->device newBufferWithLength:size options:MTLResourceStorageModeShared];return b!=nil;
        };
        if(!ensure(impl_->destination,p.destination.pixels.size()) ||
            (!resident && !ensure(impl_->source,p.source.pixels.size())) || !ensure(impl_->second,p.reciprocal_corrections.size()))return result;
        std::memcpy([impl_->destination contents],p.destination.pixels.data(),p.destination.pixels.size());
        if(!resident)std::memcpy([impl_->source contents],p.source.pixels.data(),p.source.pixels.size());
        std::memcpy([impl_->second contents],p.reciprocal_corrections.data(),p.reciprocal_corrections.size());
        if(timing)result.upload_ns=elapsed(upload);
        const auto wait=timing?Clock::now():Clock::time_point{};
        id<MTLCommandBuffer> command=[impl_->queue commandBuffer];
        id<MTLComputeCommandEncoder> encoder=[command computeCommandEncoder];
        if(!command || !encoder)return result;
        const uint32_t args[]={p.destination.stride,p.source.stride,p.destination.byte_offset,
            p.source.byte_offset,p.destination.width,p.destination.height,p.destination.format};
        [encoder setComputePipelineState:impl_->layer_pipeline];
        [encoder setBuffer:impl_->destination offset:0 atIndex:0];
        [encoder setBuffer:resident?resident->buffer:impl_->source offset:0 atIndex:1];
        [encoder setBuffer:impl_->second offset:0 atIndex:2];
        [encoder setBytes:args length:sizeof(args) atIndex:3];
        [encoder dispatchThreads:MTLSizeMake((p.destination.width+1U)/2U,p.destination.height,1)
            threadsPerThreadgroup:MTLSizeMake(16,16,1)];
        [encoder endEncoding];[command commit];[command waitUntilCompleted];
        if(timing)result.wait_ns=elapsed(wait);
        if([command status]!=MTLCommandBufferStatusCompleted)return result;
        const auto download=timing?Clock::now():Clock::time_point{};
        std::memcpy(p.destination.pixels.data(),[impl_->destination contents],p.destination.pixels.size());
        if(timing){result.download_ns=elapsed(download);result.total_ns=elapsed(start);}
        result.success=true;result.used_gpu=true;return result;
    }
}

PixelKernelResult GpuPixelKernels::layer_composite_batch(std::span<const LayerCompositePixels> layers) {
    PixelKernelResult result;
    if(!available() || !impl_->layer_pipeline || !valid_layer_composite_batch(layers))return result;
    std::vector<std::shared_ptr<MetalPixelStorage>> sources;sources.reserve(layers.size());
    for(const auto& p:layers) {
        auto source=metal_storage(p.source_storage);
        if(!source || source->device!=impl_->device)return result;
        sources.push_back(std::move(source));
    }
    const bool timing=debug_enabled();const auto start=timing?Clock::now():Clock::time_point{};
    @autoreleasepool {
        const auto& first=layers.front();
        const auto upload=timing?Clock::now():Clock::time_point{};
        const auto ensure=[&](id<MTLBuffer>& b,size_t size) {
            if(b && [b length]>=size)return true;
            [b release];b=[impl_->device newBufferWithLength:size options:MTLResourceStorageModeShared];return b!=nil;
        };
        if(!ensure(impl_->destination,first.destination.pixels.size()) || !ensure(impl_->second,65536))return result;
        std::memcpy([impl_->destination contents],first.destination.pixels.data(),first.destination.pixels.size());
        std::memcpy([impl_->second contents],first.reciprocal_corrections.data(),65536);
        if(timing)result.upload_ns=elapsed(upload);
        const auto wait=timing?Clock::now():Clock::time_point{};
        id<MTLCommandBuffer> command=[impl_->queue commandBuffer];if(!command)return result;
        for(size_t i=0;i<layers.size();++i) {
            const auto& p=layers[i];
            // Separate encoders preserve read-after-write ordering for
            // overlapping icons without a CPU round trip between them.
            id<MTLComputeCommandEncoder> encoder=[command computeCommandEncoder];if(!encoder)return result;
            const uint32_t args[]={p.destination.stride,p.source.stride,p.destination.byte_offset,
                p.source.byte_offset,p.destination.width,p.destination.height,p.destination.format};
            [encoder setComputePipelineState:impl_->layer_pipeline];
            [encoder setBuffer:impl_->destination offset:0 atIndex:0];
            [encoder setBuffer:sources[i]->buffer offset:0 atIndex:1];
            [encoder setBuffer:impl_->second offset:0 atIndex:2];
            [encoder setBytes:args length:sizeof(args) atIndex:3];
            [encoder dispatchThreads:MTLSizeMake((p.destination.width+1U)/2U,p.destination.height,1)
                threadsPerThreadgroup:MTLSizeMake(16,16,1)];
            [encoder endEncoding];
        }
        [command commit];[command waitUntilCompleted];
        if(timing)result.wait_ns=elapsed(wait);
        if([command status]!=MTLCommandBufferStatusCompleted)return result;
        const auto download=timing?Clock::now():Clock::time_point{};
        std::memcpy(first.destination.pixels.data(),[impl_->destination contents],first.destination.pixels.size());
        if(timing){result.download_ns=elapsed(download);result.total_ns=elapsed(start);}
        result.success=true;result.used_gpu=true;return result;
    }
}

PixelKernelResult GpuPixelKernels::pixel_transfer(const PixelTransferPixels& p) {
    PixelKernelResult result;
    if(!available() || !impl_->transfer_pipeline || !valid_pixel_transfer(p))return result;
    const bool timing=debug_enabled();const auto start=timing?Clock::now():Clock::time_point{};
    const auto resident=metal_storage(p.source_storage);
    if(p.source_storage && (!resident || resident->device!=impl_->device))return result;
    const bool needs_source=p.operation!=PixelTransferOperation::ColorFilter && p.operation!=PixelTransferOperation::Clear;
    @autoreleasepool {
        const auto upload=timing?Clock::now():Clock::time_point{};
        const auto ensure=[&](id<MTLBuffer>& b,size_t size) {
            if(b && [b length]>=size)return true;
            [b release];b=[impl_->device newBufferWithLength:size options:MTLResourceStorageModeShared];return b!=nil;
        };
        if(!ensure(impl_->destination,p.destination.pixels.size()) ||
            (!resident && !ensure(impl_->source,needs_source?p.source.pixels.size():4U)))return result;
        std::memcpy([impl_->destination contents],p.destination.pixels.data(),p.destination.pixels.size());
        if(needs_source && !resident)std::memcpy([impl_->source contents],p.source.pixels.data(),p.source.pixels.size());
        if(timing)result.upload_ns=elapsed(upload);
        const auto wait=timing?Clock::now():Clock::time_point{};
        id<MTLCommandBuffer> command=[impl_->queue commandBuffer];
        id<MTLComputeCommandEncoder> encoder=[command computeCommandEncoder];
        if(!command || !encoder)return result;
        const uint32_t args[]={p.destination.stride,p.source.stride,p.destination.byte_offset,
            p.source.byte_offset,p.destination.width,p.destination.height,p.destination.format,p.source.format,
            static_cast<uint32_t>(p.operation),p.parameter,p.color};
        [encoder setComputePipelineState:impl_->transfer_pipeline];
        [encoder setBuffer:impl_->destination offset:0 atIndex:0];
        [encoder setBuffer:resident?resident->buffer:impl_->source offset:0 atIndex:1];
        [encoder setBytes:args length:sizeof(args) atIndex:2];
        [encoder dispatchThreads:MTLSizeMake(p.destination.width,p.destination.height,1)
            threadsPerThreadgroup:MTLSizeMake(16,16,1)];
        [encoder endEncoding];[command commit];[command waitUntilCompleted];
        if(timing)result.wait_ns=elapsed(wait);
        if([command status]!=MTLCommandBufferStatusCompleted)return result;
        const auto download=timing?Clock::now():Clock::time_point{};
        std::memcpy(p.destination.pixels.data(),[impl_->destination contents],p.destination.pixels.size());
        if(timing){result.download_ns=elapsed(download);result.total_ns=elapsed(start);}
        result.success=true;result.used_gpu=true;return result;
    }
}

PixelKernelResult GpuPixelKernels::effector_blur(const EffectorBlurPixels& p) {
    PixelKernelResult result;
    if(!available() || !impl_->blur_horizontal || !impl_->blur_vertical || !valid_effector_blur(p))return result;
    if(!p.radius) {
        if(p.destination.pixels.data()==p.source.pixels.data() && p.destination.byte_offset==p.source.byte_offset &&
                p.destination.stride==p.source.stride) {result.success=true;return result;}
        return pixel_transfer({p.destination,p.source,PixelTransferOperation::Copy});
    }
    if(p.selector<4) {
        const auto d=reinterpret_cast<uintptr_t>(p.destination.pixels.data()),s=reinterpret_cast<uintptr_t>(p.source.pixels.data());
        if(d<=s?s-d<p.destination.pixels.size():d-s<p.source.pixels.size())return result;
    }
    const bool timing=debug_enabled();const auto start=timing?Clock::now():Clock::time_point{};
    @autoreleasepool {
        const auto upload=timing?Clock::now():Clock::time_point{};
        const auto ensure=[&](id<MTLBuffer>& b,size_t size) {
            if(b && [b length]>=size)return true;
            [b release];b=[impl_->device newBufferWithLength:size options:MTLResourceStorageModeShared];return b!=nil;
        };
        if(!ensure(impl_->destination,p.destination.pixels.size()) || !ensure(impl_->source,p.source.pixels.size()) ||
                (p.selector>=4 && !ensure(impl_->second,uint64_t(p.source.width)*p.source.height*4)))return result;
        std::memcpy([impl_->destination contents],p.destination.pixels.data(),p.destination.pixels.size());
        std::memcpy([impl_->source contents],p.source.pixels.data(),p.source.pixels.size());
        if(timing)result.upload_ns=elapsed(upload);
        const uint32_t radius=std::min(p.radius,255U),diameter=radius*2+1;
        const bool per_pixel=radius<=8 && impl_->blur_horizontal_pixels && impl_->blur_vertical_pixels;
        const auto horizontal=per_pixel?impl_->blur_horizontal_pixels:impl_->blur_horizontal;
        const auto vertical=per_pixel?impl_->blur_vertical_pixels:impl_->blur_vertical;
        const uint32_t args[]={p.destination.stride,p.source.stride,p.destination.byte_offset,p.source.byte_offset,
            p.source.width,p.source.height,radius,((0x20000U+diameter*2)/diameter)&0xffffU,p.selector};
        const auto wait=timing?Clock::now():Clock::time_point{};
        id<MTLCommandBuffer> command=[impl_->queue commandBuffer];
        if(!command)return result;
        const auto pass=[&](id<MTLComputePipelineState> pipeline,id<MTLBuffer> destination,id<MTLBuffer> source,uint32_t count) {
            id<MTLComputeCommandEncoder> encoder=[command computeCommandEncoder];
            if(!encoder)return false;
            [encoder setComputePipelineState:pipeline];
            [encoder setBuffer:destination offset:0 atIndex:0];[encoder setBuffer:source offset:0 atIndex:1];
            [encoder setBytes:args length:sizeof(args) atIndex:2];
            if(per_pixel)
                [encoder dispatchThreads:MTLSizeMake(p.source.width,p.source.height,1) threadsPerThreadgroup:MTLSizeMake(16,8,1)];
            else
                [encoder dispatchThreads:MTLSizeMake(count,1,1) threadsPerThreadgroup:MTLSizeMake(std::min<NSUInteger>(256,[pipeline maxTotalThreadsPerThreadgroup]),1,1)];
            [encoder endEncoding];return true;
        };
        if(p.selector<2) {if(!pass(horizontal,impl_->destination,impl_->source,p.source.height))return result;}
        else if(p.selector<4) {if(!pass(vertical,impl_->destination,impl_->source,p.source.width))return result;}
        else if(!pass(horizontal,impl_->second,impl_->source,p.source.height) ||
                !pass(vertical,impl_->destination,impl_->second,p.source.width))return result;
        [command commit];[command waitUntilCompleted];
        if(timing)result.wait_ns=elapsed(wait);
        if([command status]!=MTLCommandBufferStatusCompleted)return result;
        const auto download=timing?Clock::now():Clock::time_point{};
        std::memcpy(p.destination.pixels.data(),[impl_->destination contents],p.destination.pixels.size());
        if(timing){result.download_ns=elapsed(download);result.total_ns=elapsed(start);}
        result.success=true;result.used_gpu=true;return result;
    }
}

PixelKernelResult GpuPixelKernels::effector_displacement(const EffectorDisplacementPixels& p) {
    PixelKernelResult result;
    if(!available() || !impl_->displacement_pipeline || !valid_effector_displacement(p))return result;
    const bool timing=debug_enabled();const auto start=timing?Clock::now():Clock::time_point{};
    @autoreleasepool {
        const auto upload=timing?Clock::now():Clock::time_point{};
        const auto put=[&](id<MTLBuffer>& b,std::span<const uint8_t> bytes) {
            if(!b || [b length]<bytes.size()) {[b release];b=[impl_->device newBufferWithLength:bytes.size() options:MTLResourceStorageModeShared];}
            if(!b)return false;std::memcpy([b contents],bytes.data(),bytes.size());return true;
        };
        if(!put(impl_->destination,p.destination.pixels) || !put(impl_->source,p.source.pixels) || !put(impl_->second,p.first.pixels) ||
                (p.second && !put(impl_->auxiliary,p.second->pixels)))return result;
        if(timing)result.upload_ns=elapsed(upload);
        const auto wait=timing?Clock::now():Clock::time_point{};
        id<MTLCommandBuffer> command=[impl_->queue commandBuffer];id<MTLComputeCommandEncoder> encoder=[command computeCommandEncoder];
        if(!command || !encoder)return result;
        const uint32_t args[]={p.destination.width,p.destination.height,p.destination.stride,p.source.stride,p.first.stride,
            p.second?p.second->stride:0,p.destination.byte_offset,p.source.byte_offset,p.first.byte_offset,p.second?p.second->byte_offset:0,
            p.strength,p.bilinear?1U:0U,p.second?1U:0U,p.source.width,p.source.height};
        [encoder setComputePipelineState:impl_->displacement_pipeline];
        [encoder setBuffer:impl_->destination offset:0 atIndex:0];[encoder setBuffer:impl_->source offset:0 atIndex:1];
        [encoder setBuffer:impl_->second offset:0 atIndex:2];[encoder setBuffer:p.second?impl_->auxiliary:impl_->second offset:0 atIndex:3];
        [encoder setBytes:args length:sizeof(args) atIndex:4];
        [encoder dispatchThreads:MTLSizeMake(p.destination.width,p.destination.height,1) threadsPerThreadgroup:MTLSizeMake(16,16,1)];
        [encoder endEncoding];[command commit];[command waitUntilCompleted];
        if(timing)result.wait_ns=elapsed(wait);
        if([command status]!=MTLCommandBufferStatusCompleted)return result;
        const auto download=timing?Clock::now():Clock::time_point{};
        std::memcpy(p.destination.pixels.data(),[impl_->destination contents],p.destination.pixels.size());
        if(timing){result.download_ns=elapsed(download);result.total_ns=elapsed(start);}
        result.success=true;result.used_gpu=true;return result;
    }
}

PixelKernelResult GpuPixelKernels::effector_vector(const EffectorVectorPixels& p) {
    PixelKernelResult result;
    if(!available() || !impl_->vector_pipeline || !valid_effector_vector(p))return result;
    const bool timing=debug_enabled();const auto start=timing?Clock::now():Clock::time_point{};
    @autoreleasepool {
        const auto upload=timing?Clock::now():Clock::time_point{};
        const auto put=[&](id<MTLBuffer>& b,std::span<const uint8_t> bytes) {
            if(!b || [b length]<bytes.size()) {[b release];b=[impl_->device newBufferWithLength:bytes.size() options:MTLResourceStorageModeShared];}
            if(!b)return false;std::memcpy([b contents],bytes.data(),bytes.size());return true;
        };
        const std::span<const uint8_t> coefficients(reinterpret_cast<const uint8_t*>(p.coefficients.data()),p.coefficients.size_bytes());
        if(!put(impl_->destination,p.destination.pixels) || !put(impl_->source,p.source.pixels) || !put(impl_->second,p.normal.pixels) ||
                !put(impl_->auxiliary,coefficients))return result;
        if(timing)result.upload_ns=elapsed(upload);
        const auto wait=timing?Clock::now():Clock::time_point{};
        id<MTLCommandBuffer> command=[impl_->queue commandBuffer];id<MTLComputeCommandEncoder> encoder=[command computeCommandEncoder];
        if(!command || !encoder)return result;
        const uint32_t args[]={p.destination.width,p.destination.height,p.destination.stride,p.source.stride,p.normal.stride,
            p.destination.byte_offset,p.source.byte_offset,p.normal.byte_offset,p.bilinear?1U:0U,p.rectangular_bounds?1U:0U};
        [encoder setComputePipelineState:impl_->vector_pipeline];
        [encoder setBuffer:impl_->destination offset:0 atIndex:0];[encoder setBuffer:impl_->source offset:0 atIndex:1];
        [encoder setBuffer:impl_->second offset:0 atIndex:2];[encoder setBuffer:impl_->auxiliary offset:0 atIndex:3];
        [encoder setBytes:args length:sizeof(args) atIndex:4];
        [encoder dispatchThreads:MTLSizeMake(p.destination.width,p.destination.height,1) threadsPerThreadgroup:MTLSizeMake(16,16,1)];
        [encoder endEncoding];[command commit];[command waitUntilCompleted];
        if(timing)result.wait_ns=elapsed(wait);
        if([command status]!=MTLCommandBufferStatusCompleted)return result;
        const auto download=timing?Clock::now():Clock::time_point{};
        std::memcpy(p.destination.pixels.data(),[impl_->destination contents],p.destination.pixels.size());
        if(timing){result.download_ns=elapsed(download);result.total_ns=elapsed(start);}
        result.success=true;result.used_gpu=true;return result;
    }
}

PixelKernelResult GpuPixelKernels::effector_transform(const EffectorTransformPixels& p) {
    PixelKernelResult result;
    if(!available() || !impl_->transform_pipeline || !valid_effector_transform(p))return result;
    const bool timing=debug_enabled();const auto start=timing?Clock::now():Clock::time_point{};
    @autoreleasepool {
        const auto upload=timing?Clock::now():Clock::time_point{};
        const auto put=[&](id<MTLBuffer>& b,std::span<const uint8_t> bytes) {
            if(!b || [b length]<bytes.size()) {[b release];b=[impl_->device newBufferWithLength:bytes.size() options:MTLResourceStorageModeShared];}
            if(!b)return false;std::memcpy([b contents],bytes.data(),bytes.size());return true;
        };
        if(!put(impl_->destination,p.destination.pixels) || !put(impl_->source,p.source.pixels))return result;
        if(timing)result.upload_ns=elapsed(upload);
        const auto wait=timing?Clock::now():Clock::time_point{};
        id<MTLCommandBuffer> command=[impl_->queue commandBuffer];id<MTLComputeCommandEncoder> encoder=[command computeCommandEncoder];
        if(!command || !encoder)return result;
        const uint32_t args[]={p.destination.width,p.destination.height,p.destination.stride,p.source.width,p.source.height,p.source.stride,
            p.destination.byte_offset,p.source.byte_offset,uint32_t(p.matrix[0]),uint32_t(p.matrix[1]),uint32_t(p.matrix[2]),
            uint32_t(p.matrix[3]),uint32_t(p.matrix[4]),uint32_t(p.matrix[5]),p.bilinear?1U:0U};
        [encoder setComputePipelineState:impl_->transform_pipeline];
        [encoder setBuffer:impl_->destination offset:0 atIndex:0];[encoder setBuffer:impl_->source offset:0 atIndex:1];
        [encoder setBytes:args length:sizeof(args) atIndex:2];
        [encoder dispatchThreads:MTLSizeMake(p.destination.width,p.destination.height,1) threadsPerThreadgroup:MTLSizeMake(16,16,1)];
        [encoder endEncoding];[command commit];[command waitUntilCompleted];
        if(timing)result.wait_ns=elapsed(wait);
        if([command status]!=MTLCommandBufferStatusCompleted)return result;
        const auto download=timing?Clock::now():Clock::time_point{};
        std::memcpy(p.destination.pixels.data(),[impl_->destination contents],p.destination.pixels.size());
        if(timing){result.download_ns=elapsed(download);result.total_ns=elapsed(start);}
        result.success=true;result.used_gpu=true;return result;
    }
}
PixelKernelResult GpuPixelKernels::effector_temporal(const EffectorTemporalPixels& p) {
    PixelKernelResult result;
    if(!available() || !impl_->temporal_pipeline || !impl_->temporal_retain_pipeline || !valid_effector_temporal(p))return result;
    const bool timing=debug_enabled();const auto start=timing?Clock::now():Clock::time_point{};
    @autoreleasepool {
        const auto upload=timing?Clock::now():Clock::time_point{};
        const auto put=[&](id<MTLBuffer>& b,std::span<const uint8_t> bytes) {
            if(!b || [b length]<bytes.size()) {[b release];b=[impl_->device newBufferWithLength:bytes.size() options:MTLResourceStorageModeShared];}
            if(!b)return false;std::memcpy([b contents],bytes.data(),bytes.size());return true;
        };
        const auto& table=detail::kRcpSsEstimateTable;
        const std::span<const uint8_t> rcp(reinterpret_cast<const uint8_t*>(table.data()),sizeof(table));
        if(!put(impl_->destination,p.scene.pixels) || !put(impl_->source,p.history.pixels) || !put(impl_->auxiliary,rcp))return result;
        if(timing)result.upload_ns=elapsed(upload);
        const auto wait=timing?Clock::now():Clock::time_point{};
        id<MTLCommandBuffer> command=[impl_->queue commandBuffer];if(!command)return result;
        const uint32_t args[]={p.scene.stride,p.history.stride,p.scene.byte_offset,p.history.byte_offset,p.scene.width,p.scene.height,
            p.history.width,p.history.height,p.scene.format,p.strength};
        if(p.strength && p.strength<=256) {
            id<MTLComputeCommandEncoder> encoder=[command computeCommandEncoder];if(!encoder)return result;
            [encoder setComputePipelineState:impl_->temporal_pipeline];
            [encoder setBuffer:impl_->destination offset:0 atIndex:0];[encoder setBuffer:impl_->source offset:0 atIndex:1];
            [encoder setBuffer:impl_->auxiliary offset:0 atIndex:2];[encoder setBytes:args length:sizeof(args) atIndex:3];
            [encoder dispatchThreads:MTLSizeMake((p.history.width+1)/2,p.history.height,1) threadsPerThreadgroup:MTLSizeMake(16,16,1)];
            [encoder endEncoding];
        }
        id<MTLComputeCommandEncoder> encoder=[command computeCommandEncoder];if(!encoder)return result;
        [encoder setComputePipelineState:impl_->temporal_retain_pipeline];
        [encoder setBuffer:impl_->destination offset:0 atIndex:0];[encoder setBuffer:impl_->source offset:0 atIndex:1];
        [encoder setBytes:args length:sizeof(args) atIndex:2];
        [encoder dispatchThreads:MTLSizeMake(p.scene.width,p.scene.height,1) threadsPerThreadgroup:MTLSizeMake(16,16,1)];
        [encoder endEncoding];[command commit];[command waitUntilCompleted];
        if(timing)result.wait_ns=elapsed(wait);
        if([command status]!=MTLCommandBufferStatusCompleted)return result;
        const auto download=timing?Clock::now():Clock::time_point{};
        std::memcpy(p.scene.pixels.data(),[impl_->destination contents],p.scene.pixels.size());
        std::memcpy(p.history.pixels.data(),[impl_->source contents],p.history.pixels.size());
        if(timing){result.download_ns=elapsed(download);result.total_ns=elapsed(start);}
        result.success=true;result.used_gpu=true;return result;
    }
}

std::shared_ptr<GpuPixelStorage> GpuPixelKernels::store_pixels(std::span<const uint8_t> pixels) {
    const std::array<std::span<const uint8_t>,1> upload{pixels};
    auto stored=store_pixels_batch(upload);
    return stored.empty()?std::shared_ptr<GpuPixelStorage>{}:std::move(stored[0]);
}
std::vector<std::shared_ptr<GpuPixelStorage>> GpuPixelKernels::store_pixels_batch(std::span<const std::span<const uint8_t>> pixels) {
    std::vector<std::shared_ptr<GpuPixelStorage>> result(pixels.size());
    if(!available())return result;
    const char* private_pixels=std::getenv("BGI_METAL_PRIVATE_PIXELS");
    if([impl_->device hasUnifiedMemory] && !(private_pixels && std::strcmp(private_pixels,"1")==0)) {
        @autoreleasepool {
            for(size_t n=0;n<pixels.size();++n) {
                const auto bytes=pixels[n];if(bytes.empty())continue;
                id<MTLBuffer> buffer=[impl_->device newBufferWithBytes:bytes.data() length:bytes.size()
                    options:MTLResourceStorageModeShared];
                if(buffer)result[n]=std::make_shared<MetalPixelStorage>(impl_->device,impl_->queue,buffer);
                [buffer release];
            }
        }
        return result;
    }
    // One wait for a bounded group, rather than one GPU round trip per bitmap.
    // Cap transient staging to32MiB unless a single resource is larger.
    for(size_t first=0;first<pixels.size();) {
        size_t last=first,total=0;
        do {total+=pixels[last++].size();} while(last<pixels.size() &&
            pixels[last].size()<=32U*1024U*1024U && total<=32U*1024U*1024U-pixels[last].size());
        @autoreleasepool {
            id<MTLCommandBuffer> command=[impl_->queue commandBuffer];
            id<MTLBlitCommandEncoder> encoder=[command blitCommandEncoder];
            if(!command || !encoder)return result;
            std::vector<std::shared_ptr<GpuPixelStorage>> group(last-first);
            bool good=true;
            for(size_t n=first;n<last;++n) {
                const auto bytes=pixels[n];if(bytes.empty())continue;
                id<MTLBuffer> buffer=[impl_->device newBufferWithLength:bytes.size() options:MTLResourceStorageModePrivate];
                id<MTLBuffer> staging=[impl_->device newBufferWithBytes:bytes.data() length:bytes.size() options:MTLResourceStorageModeShared];
                if(!buffer || !staging)good=false;
                else {
                    [encoder copyFromBuffer:staging sourceOffset:0 toBuffer:buffer destinationOffset:0 size:bytes.size()];
                    group[n-first]=std::make_shared<MetalPixelStorage>(impl_->device,impl_->queue,buffer);
                }
                [buffer release];[staging release];
                if(!good)break;
            }
            [encoder endEncoding];
            if(!good)return result;
            [command commit];[command waitUntilCompleted];
            if([command status]!=MTLCommandBufferStatusCompleted)return result;
            for(size_t n=first;n<last;++n)result[n]=std::move(group[n-first]);
        }
        first=last;
    }
    return result;
}

}

namespace bgiplayer {
namespace {
// Reuse the exact transfer/layer shaders with an immutable GPU destination.
// Each result has independent ownership; rejection never changes either input.
template<class State>
std::shared_ptr<GpuPixelStorage> resident_bitmap_operation(State& state,
    const PixelTransferPixels& p,std::shared_ptr<GpuPixelStorage> previous,
    std::span<const uint8_t> corrections,bool layer) {
    const auto dst=metal_storage(previous);
    const auto src=metal_storage(p.source_storage);
    const auto valid=[](const GlyphBitmapView& v,size_t bytes) {
        return v.pixels.empty() && v.width && v.height && v.bytes_per_pixel==4 &&
            (v.format==1 || v.format==2) && bytes<=UINT32_MAX &&
            v.stride>=uint64_t(v.width)*4 &&
            uint64_t(v.byte_offset)+uint64_t(v.height-1)*v.stride+uint64_t(v.width)*4<=bytes;
    };
    if(!dst || !src || !state.device || !state.transfer_pipeline || !state.layer_pipeline ||
        dst->device!=state.device || src->device!=state.device ||
        !valid(p.destination,dst->size()) || !valid(p.source,src->size()) ||
        p.destination.width!=p.source.width || p.destination.height!=p.source.height || p.parameter>256)return {};
    if(layer) {if(p.source.format!=2 || corrections.size()!=65536)return {};}
    else switch(p.operation) {
        case PixelTransferOperation::Copy:break;
        case PixelTransferOperation::FadeSource:
        case PixelTransferOperation::BlendOpaque:
            if(p.destination.format!=1 || p.source.format!=1)return {};break;
        case PixelTransferOperation::BlendAlpha:
            if(p.destination.format!=1 || p.source.format!=2 || !p.parameter || p.parameter>=256)return {};break;
        default:return {};
    }
    const char* forced=std::getenv("BGI_GPU");
    if(!(forced && std::strcmp(forced,"1")==0) && !layer && p.operation==PixelTransferOperation::Copy &&
        p.destination.format==p.source.format && p.destination.byte_offset==0 && p.source.byte_offset==0 &&
        p.destination.stride==p.source.stride && p.destination.stride==uint64_t(p.destination.width)*4 &&
        dst->size()==src->size() && uint64_t(p.destination.height)*p.destination.stride==dst->size())return p.source_storage;
    @autoreleasepool {
        id<MTLBuffer> output=[state.device newBufferWithLength:dst->size() options:MTLResourceStorageModeShared];
        if(!output)return {};
        id<MTLCommandBuffer> command=[state.queue commandBuffer];
        id<MTLBlitCommandEncoder> copy=[command blitCommandEncoder];
        if(!command || !copy){[output release];return {};}
        [copy copyFromBuffer:dst->buffer sourceOffset:0 toBuffer:output destinationOffset:0 size:dst->size()];
        [copy endEncoding];
        id<MTLComputeCommandEncoder> encoder=[command computeCommandEncoder];
        if(!encoder){[output release];return {};}
        [encoder setBuffer:output offset:0 atIndex:0];
        [encoder setBuffer:src->buffer offset:0 atIndex:1];
        if(layer) {
            if(!state.second || [state.second length]<corrections.size()) {
                [state.second release];state.second=[state.device newBufferWithLength:corrections.size() options:MTLResourceStorageModeShared];
            }
            if(!state.second){[encoder endEncoding];[output release];return {};}
            std::memcpy([state.second contents],corrections.data(),corrections.size());
            const uint32_t args[]={p.destination.stride,p.source.stride,p.destination.byte_offset,
                p.source.byte_offset,p.destination.width,p.destination.height,p.destination.format};
            [encoder setComputePipelineState:state.layer_pipeline];
            [encoder setBuffer:state.second offset:0 atIndex:2];
            [encoder setBytes:args length:sizeof(args) atIndex:3];
            [encoder dispatchThreads:MTLSizeMake((p.destination.width+1U)/2U,p.destination.height,1)
                threadsPerThreadgroup:MTLSizeMake(16,16,1)];
        } else {
            const uint32_t args[]={p.destination.stride,p.source.stride,p.destination.byte_offset,
                p.source.byte_offset,p.destination.width,p.destination.height,p.destination.format,p.source.format,
                static_cast<uint32_t>(p.operation),p.parameter,p.color};
            [encoder setComputePipelineState:state.transfer_pipeline];
            [encoder setBytes:args length:sizeof(args) atIndex:2];
            [encoder dispatchThreads:MTLSizeMake(p.destination.width,p.destination.height,1)
                threadsPerThreadgroup:MTLSizeMake(16,16,1)];
        }
        [encoder endEncoding];[command commit];[command waitUntilCompleted];
        std::shared_ptr<GpuPixelStorage> result;
        if([command status]==MTLCommandBufferStatusCompleted)
            result=std::make_shared<MetalPixelStorage>(state.device,state.queue,output);
        [output release];return result;
    }
}
}
std::shared_ptr<GpuPixelStorage> GpuPixelKernels::resident_transfer(
    const PixelTransferPixels& p,std::shared_ptr<GpuPixelStorage> destination) {
    return resident_bitmap_operation(*impl_,p,std::move(destination),{},false);
}
std::shared_ptr<GpuPixelStorage> GpuPixelKernels::resident_layer(
    const LayerCompositePixels& p,std::shared_ptr<GpuPixelStorage> destination) {
    return resident_bitmap_operation(*impl_,{p.destination,p.source,PixelTransferOperation::Copy,0,0,p.source_storage},
        std::move(destination),p.reciprocal_corrections,true);
}
}

namespace bgiplayer {
std::shared_ptr<GpuPixelStorage> GpuPixelKernels::filled_pixels(size_t bytes,uint32_t value) {
    if(!available() || !bytes || ![impl_->device hasUnifiedMemory])return {};
    @autoreleasepool {
        id<MTLBuffer> buffer=[impl_->device newBufferWithLength:bytes options:MTLResourceStorageModeShared];
        if(!buffer)return {};
        auto* words=static_cast<uint32_t*>([buffer contents]);
        std::fill_n(words,bytes/4,value);
        auto* tail=static_cast<uint8_t*>([buffer contents])+bytes/4*4;
        std::fill_n(tail,bytes%4,uint8_t{0});
        auto result=std::make_shared<MetalPixelStorage>(impl_->device,impl_->queue,buffer);
        [buffer release];return result;
    }
}
std::shared_ptr<GpuPixelStorage> GpuPixelKernels::resident_layers(
    std::span<const LayerCompositePixels> layers,std::shared_ptr<GpuPixelStorage> previous) {
    const auto dst=metal_storage(previous);
    if(!available() || !dst || dst->device!=impl_->device || layers.size()<2 || layers.size()>256)return {};
    const auto valid=[](const GlyphBitmapView& v,size_t bytes) {
        return v.pixels.empty() && v.width && v.height && v.bytes_per_pixel==4 && bytes<=UINT32_MAX &&
            v.stride>=uint64_t(v.width)*4 && uint64_t(v.byte_offset)+uint64_t(v.height-1)*v.stride+uint64_t(v.width)*4<=bytes;
    };
    std::vector<std::shared_ptr<MetalPixelStorage>> sources;sources.reserve(layers.size());
    const auto& first=layers.front();
    for(const auto& p:layers) {
        auto source=metal_storage(p.source_storage);
        if(!source || source->device!=impl_->device || p.source.format!=2 ||
            (p.destination.format!=1 && p.destination.format!=2) || p.destination.format!=first.destination.format ||
            p.destination.stride!=first.destination.stride || p.reciprocal_corrections.size()!=65536 ||
            p.reciprocal_corrections.data()!=first.reciprocal_corrections.data() ||
            p.source.width!=p.destination.width || p.source.height!=p.destination.height ||
            !valid(p.source,source->size()) || !valid(p.destination,dst->size()))return {};
        sources.push_back(std::move(source));
    }
    @autoreleasepool {
        id<MTLBuffer> output=[impl_->device newBufferWithLength:dst->size() options:MTLResourceStorageModeShared];
        id<MTLBuffer> corrections=[impl_->device newBufferWithBytes:first.reciprocal_corrections.data()
            length:65536 options:MTLResourceStorageModeShared];
        if(!output || !corrections){[output release];[corrections release];return {};}
        id<MTLCommandBuffer> command=[impl_->queue commandBuffer];
        id<MTLBlitCommandEncoder> copy=[command blitCommandEncoder];
        if(!command || !copy){[output release];[corrections release];return {};}
        [copy copyFromBuffer:dst->buffer sourceOffset:0 toBuffer:output destinationOffset:0 size:dst->size()];
        [copy endEncoding];
        bool good=true;
        for(size_t n=0;n<layers.size();++n) {
            const auto& p=layers[n];
            id<MTLComputeCommandEncoder> encoder=[command computeCommandEncoder];
            if(!encoder){good=false;break;}
            const uint32_t args[]={p.destination.stride,p.source.stride,p.destination.byte_offset,
                p.source.byte_offset,p.destination.width,p.destination.height,p.destination.format};
            [encoder setComputePipelineState:impl_->layer_pipeline];
            [encoder setBuffer:output offset:0 atIndex:0];
            [encoder setBuffer:sources[n]->buffer offset:0 atIndex:1];
            [encoder setBuffer:corrections offset:0 atIndex:2];
            [encoder setBytes:args length:sizeof(args) atIndex:3];
            [encoder dispatchThreads:MTLSizeMake((p.destination.width+1U)/2U,p.destination.height,1)
                threadsPerThreadgroup:MTLSizeMake(16,16,1)];
            [encoder endEncoding];
        }
        std::shared_ptr<GpuPixelStorage> result;
        if(good) {
            [command commit];[command waitUntilCompleted];
            if([command status]==MTLCommandBufferStatusCompleted)
                result=std::make_shared<MetalPixelStorage>(impl_->device,impl_->queue,output);
        }
        [output release];[corrections release];return result;
    }
}
}
