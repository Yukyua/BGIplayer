#ifdef __APPLE__
#include "bgiplayer/gpu_exact_pixel_ops.hpp"
#include "bgiplayer/gpu_raw_patches.hpp"
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <chrono>
#include <cstring>
namespace bgiplayer::exact_pixel_detail {
namespace {
struct MetalState {
    id<MTLDevice> device=nil;
    id<MTLCommandQueue> queue=nil;
    id<MTLComputePipelineState> pipeline=nil;
    MetalState() {
        @autoreleasepool {
            device=MTLCreateSystemDefaultDevice();if(!device)return;
            queue=[device newCommandQueue];if(!queue)return;
            std::string shader=R"MSL(
#include <metal_stdlib>
using namespace metal;
kernel void exact_pixels(device uchar* _exact_d [[buffer(0)]],device const uchar* _exact_a [[buffer(1)]],device const uchar* _exact_b [[buffer(2)]],constant uint* _exact_p [[buffer(3)]],uint2 t [[thread_position_in_grid]]) {
uint x=t.x,y=t.y;
#define p _exact_p
#define A(i) uint(_exact_a[uint(i)])
#define B(i) uint(_exact_b[uint(i)])
#define D(i) uint(_exact_d[uint(i)])
#define W(i,v) _exact_d[uint(i)]=uchar(v)
)MSL"+shader_body()+"\n}\n";
            NSError* error=nil;
            MTLCompileOptions* options=[[MTLCompileOptions alloc]init];options.fastMathEnabled=NO;
            id<MTLLibrary> library=[device newLibraryWithSource:[NSString stringWithUTF8String:shader.c_str()] options:options error:&error];
            [options release];
            if(!library){NSLog(@"Exact GPU shader: %@",error);return;}
            id<MTLFunction> function=[library newFunctionWithName:@"exact_pixels"];
            pipeline=[device newComputePipelineStateWithFunction:function error:&error];
            [function release];[library release];
        }
    }
    ~MetalState(){[pipeline release];[queue release];[device release];}
};
MetalState& state(){static thread_local MetalState s;return s;}
class ExactMetalStorage final:public GpuPixelStorage {
    id<MTLBuffer> buffer_;id<MTLCommandQueue> queue_;size_t bytes_;
public:
    ExactMetalStorage(id<MTLBuffer> b,id<MTLCommandQueue> q,size_t bytes):buffer_([b retain]),queue_([q retain]),bytes_(bytes){}
    ~ExactMetalStorage() override{[buffer_ release];[queue_ release];}
    size_t size() const override{return bytes_;}
    bool read(std::span<uint8_t> out)const override{return out.size()==bytes_ && read_range(0,out);}
    bool fast_ranges()const override{return true;}
    bool read_range(size_t offset,std::span<uint8_t> out)const override {
        if(offset>bytes_ || out.size()>bytes_-offset)return false;
        std::memcpy(out.data(),static_cast<const uint8_t*>(buffer_.contents)+offset,out.size());return true;
    }
    const void* native_handle(std::string_view kind)const override {
        if(kind=="metal-buffer")return static_cast<const void*>(buffer_);
        if(kind=="metal-queue")return static_cast<const void*>(queue_);
        return nullptr;
    }
};
id<MTLBuffer> input(MetalState& s,const GlyphBitmapView& v,const std::shared_ptr<GpuPixelStorage>& storage) {
    if(storage) {
        auto owner=storage;
        if(auto concrete=owner->concrete_storage())owner=std::move(concrete);
        if(const void* native=owner->native_handle("metal-buffer")) {
            id<MTLBuffer> b=(id<MTLBuffer>)native;
            if(b.device==s.device && [b length]>=storage->size())return [b retain];
        }
        return nil;
    }
    if(v.pixels.empty())return [s.device newBufferWithLength:4 options:MTLResourceStorageModeShared];
    return [s.device newBufferWithBytes:v.pixels.data() length:v.pixels.size() options:MTLResourceStorageModeShared];
}
std::shared_ptr<GpuPixelStorage> run(const ExactPixelPixels& p) {
    @autoreleasepool {
        auto& s=state();if(!s.pipeline)return {};
        const size_t bytes=p.destination_storage?p.destination_storage->size():(p.destination_bytes?p.destination_bytes:p.destination.pixels.size());
        id<MTLBuffer> initial=input(s,p.destination,p.destination_storage),a=input(s,p.source,p.source_storage),b=input(s,p.second,p.second_storage);
        if(!initial || !a || !b){[initial release];[a release];[b release];return {};}
        id<MTLBuffer> out=[s.device newBufferWithLength:bytes options:MTLResourceStorageModeShared];
        if(!out){[initial release];[a release];[b release];return {};}
        const auto args=arguments(p);
        id<MTLCommandBuffer> cmd=[s.queue commandBuffer];
        if(!cmd){[out release];[initial release];[a release];[b release];return {};}
        id<MTLBlitCommandEncoder> copy=[cmd blitCommandEncoder];
        if(!copy){[out release];[initial release];[a release];[b release];return {};}
        if(!p.destination_bytes)[copy copyFromBuffer:initial sourceOffset:0 toBuffer:out destinationOffset:0 size:bytes];[copy endEncoding];
        id<MTLComputeCommandEncoder> e=[cmd computeCommandEncoder];
        if(!e){[out release];[initial release];[a release];[b release];return {};}
        [e setComputePipelineState:s.pipeline];[e setBuffer:out offset:0 atIndex:0];
        [e setBuffer:a offset:0 atIndex:1];[e setBuffer:b offset:0 atIndex:2];
        [e setBytes:args.data() length:sizeof(args) atIndex:3];
        [e dispatchThreads:MTLSizeMake(p.destination.width,p.destination.height,1) threadsPerThreadgroup:MTLSizeMake(16,8,1)];
        [e endEncoding];[cmd commit];[cmd waitUntilCompleted];
        std::shared_ptr<GpuPixelStorage> result;
        if(cmd.status==MTLCommandBufferStatusCompleted)result=std::make_shared<ExactMetalStorage>(out,s.queue,bytes);
        [out release];[initial release];[a release];[b release];return result;
    }
}
}
std::shared_ptr<GpuPixelStorage> execute_resident(const ExactPixelPixels& p){return run(p);}
PixelKernelResult execute(const ExactPixelPixels& p) {
    const auto start=std::chrono::steady_clock::now();auto result=run(p);
    if(!result || result->size()!=p.destination.pixels.size())return {};
    std::vector<uint8_t> output(result->size());if(!result->read(output))return {};
    std::copy(output.begin(),output.end(),p.destination.pixels.begin());
    return {true,true,static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-start).count())};
}
}
namespace bgiplayer::raw_patch_detail {
std::shared_ptr<GpuPixelStorage> execute(const std::shared_ptr<GpuPixelStorage>& base,
        std::span<const GpuRawPatch> patches) {
    if(!valid_gpu_raw_patches(base,patches))return {};
    @autoreleasepool {
        auto& state=exact_pixel_detail::state();if(!state.pipeline)return {};
        // Batch callers have already collected a concrete base. Never invoke
        // concrete_storage()/read() here, which would flatten each input chain.
        const auto native=[&](const std::shared_ptr<GpuPixelStorage>& owner)->id<MTLBuffer> {
            auto buffer=(id<MTLBuffer>)owner->native_handle("metal-buffer");
            auto queue=(id<MTLCommandQueue>)owner->native_handle("metal-queue");
            if(!buffer || !queue || buffer.device!=state.device || queue.device!=state.device ||
                    [buffer length]<owner->size())return nil;
            return buffer;
        };
        const auto initial=native(base);if(!initial)return {};
        std::vector<id<MTLBuffer>> sources;sources.reserve(patches.size());
        for(const auto& patch:patches) {auto buffer=native(patch.pixels);if(!buffer)return {};sources.push_back(buffer);}
        id<MTLBuffer> output=[state.device newBufferWithLength:base->size() options:MTLResourceStorageModeShared];
        if(!output)return {};
        id<MTLCommandBuffer> command=[state.queue commandBuffer];
        id<MTLBlitCommandEncoder> copy=[command blitCommandEncoder];
        if(!command || !copy){[output release];return {};}
        [copy copyFromBuffer:initial sourceOffset:0 toBuffer:output destinationOffset:0 size:base->size()];
        [copy endEncoding];
        bool good=true;
        for(size_t n=0;n<patches.size();++n) {
            const auto& p=patches[n];
            const ExactPixelPixels descriptor{
                {{},uint32_t(p.offset),uint32_t(p.stride),uint32_t(p.row_bytes),uint32_t(p.rows),3,1},
                {{},0,uint32_t(p.row_bytes),uint32_t(p.row_bytes),uint32_t(p.rows),3,1},
                {},ExactPixelOperation::RawCopy};
            const auto args=exact_pixel_detail::arguments(descriptor);
            // Separate tracked encoders serialize overlapping destination
            // writes while retaining one command submission and one wait.
            id<MTLComputeCommandEncoder> encoder=[command computeCommandEncoder];
            if(!encoder){good=false;break;}
            [encoder setComputePipelineState:state.pipeline];[encoder setBuffer:output offset:0 atIndex:0];
            [encoder setBuffer:sources[n] offset:0 atIndex:1];[encoder setBuffer:sources[n] offset:0 atIndex:2];
            [encoder setBytes:args.data() length:sizeof(args) atIndex:3];
            [encoder dispatchThreads:MTLSizeMake(p.row_bytes,p.rows,1) threadsPerThreadgroup:MTLSizeMake(16,8,1)];
            [encoder endEncoding];
        }
        std::shared_ptr<GpuPixelStorage> result;
        if(good) {
            [command commit];[command waitUntilCompleted];
            if(command.status==MTLCommandBufferStatusCompleted)
                result=std::make_shared<exact_pixel_detail::ExactMetalStorage>(output,state.queue,base->size());
        }
        [output release];return result;
    }
}
}
#endif
