#include "bgiplayer/gpu_pixel_kernels.hpp"
#include "bgiplayer/debug.hpp"
#include "bgiplayer/sprite_scaled_alpha.hpp"
#include "bgiplayer/sprite_crossfade.hpp"
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <algorithm>
#include <bit>
#include <chrono>
#include <cstring>
#include <limits>

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
struct GpuPixelKernels::Impl {
    id<MTLDevice> device=nil;
    id<MTLCommandQueue> queue=nil;
    id<MTLComputePipelineState> pipeline=nil,crossfade_pipeline=nil,affine_pipeline=nil,mask_pipeline=nil,layer_pipeline=nil,transfer_pipeline=nil;
    id<MTLBuffer> destination=nil,source=nil,second=nil;
    ~Impl() {
        [source release];[second release];[destination release];[pipeline release];[crossfade_pipeline release];[affine_pipeline release];[mask_pipeline release];[layer_pipeline release];[transfer_pipeline release];[queue release];[device release];
    }
};
GpuPixelKernels::GpuPixelKernels(bool enabled) : impl_(std::make_unique<Impl>()) {
    if(!enabled)return;
    @autoreleasepool {
        impl_->device=MTLCreateSystemDefaultDevice();
        if(!impl_->device)return;
        NSError* error=nil;
        id<MTLLibrary> library=[impl_->device newLibraryWithSource:[NSString stringWithUTF8String:shader_source] options:nil error:&error];
        if(!library)return;
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
    const bool timing=debug_enabled();
    const auto start=timing?Clock::now():Clock::time_point{};
    const auto valid=[](size_t size,uint32_t width,uint32_t height,uint32_t stride) {
        return width && height && width<=32767 && height<=32767 && stride%4U==0 &&
            stride>=uint64_t(width)*4U && size<=UINT32_MAX && size>=uint64_t(stride)*height;
    };
    const uintptr_t d=reinterpret_cast<uintptr_t>(p.destination.data()),
                    a=reinterpret_cast<uintptr_t>(p.source.data());
    const bool overlap=d<=a?a-d<p.destination.size():d-a<p.source.size();
    if(!available() || !impl_->affine_pipeline || p.parameter>256 || overlap ||
       !valid(p.destination.size(),p.destination_width,p.destination_height,p.destination_stride) ||
       !valid(p.source.size(),p.source_width,p.source_height,p.source_stride))return result;
    @autoreleasepool {
        const auto upload=timing?Clock::now():Clock::time_point{};
        const auto ensure=[&](id<MTLBuffer>& buffer,size_t size) {
            if(buffer && [buffer length]>=size)return true;
            [buffer release];buffer=[impl_->device newBufferWithLength:size options:MTLResourceStorageModeShared];
            return buffer!=nil;
        };
        if(!ensure(impl_->source,p.source.size()) || !ensure(impl_->destination,p.destination.size()))return result;
        std::memcpy([impl_->source contents],p.source.data(),p.source.size());
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
        [encoder setBuffer:impl_->source offset:0 atIndex:1];
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
    if(!available() || !impl_->layer_pipeline ||
        !valid(p.destination,p.destination.format,4) || !valid(p.source,2,4) ||
        (p.destination.format!=1 && p.destination.format!=2) ||
        p.destination.width!=p.source.width || p.destination.height!=p.source.height ||
        p.reciprocal_corrections.size()!=65536 ||
        overlaps(p.destination.pixels,p.source.pixels))return result;
    @autoreleasepool {
        const auto upload=timing?Clock::now():Clock::time_point{};
        const auto ensure=[&](id<MTLBuffer>& b,size_t size) {
            if(b && [b length]>=size)return true;
            [b release];b=[impl_->device newBufferWithLength:size options:MTLResourceStorageModeShared];return b!=nil;
        };
        if(!ensure(impl_->destination,p.destination.pixels.size()) ||
            !ensure(impl_->source,p.source.pixels.size()) || !ensure(impl_->second,p.reciprocal_corrections.size()))return result;
        std::memcpy([impl_->destination contents],p.destination.pixels.data(),p.destination.pixels.size());
        std::memcpy([impl_->source contents],p.source.pixels.data(),p.source.pixels.size());
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
        [encoder setBuffer:impl_->source offset:0 atIndex:1];
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

PixelKernelResult GpuPixelKernels::pixel_transfer(const PixelTransferPixels& p) {
    PixelKernelResult result;
    if(!available() || !impl_->transfer_pipeline || !valid_pixel_transfer(p))return result;
    const bool timing=debug_enabled();const auto start=timing?Clock::now():Clock::time_point{};
    const bool needs_source=p.operation!=PixelTransferOperation::ColorFilter && p.operation!=PixelTransferOperation::Clear;
    @autoreleasepool {
        const auto upload=timing?Clock::now():Clock::time_point{};
        const auto ensure=[&](id<MTLBuffer>& b,size_t size) {
            if(b && [b length]>=size)return true;
            [b release];b=[impl_->device newBufferWithLength:size options:MTLResourceStorageModeShared];return b!=nil;
        };
        if(!ensure(impl_->destination,p.destination.pixels.size()) ||
            !ensure(impl_->source,needs_source?p.source.pixels.size():4U))return result;
        std::memcpy([impl_->destination contents],p.destination.pixels.data(),p.destination.pixels.size());
        if(needs_source)std::memcpy([impl_->source contents],p.source.pixels.data(),p.source.pixels.size());
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
        [encoder setBuffer:impl_->source offset:0 atIndex:1];
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

}
