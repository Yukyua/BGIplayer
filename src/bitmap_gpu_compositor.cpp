#include "bgiplayer/vm.hpp"
#include "bgiplayer/gpu_pixel_kernels.hpp"
#include <iostream>
namespace bgiplayer {
bool VM::accelerate_bitmap_transfer(uint32_t destination,uint32_t source,int32_t x,int32_t y,
                                    uint32_t mode,uint32_t parameter,bool direct_copy,bool source_fade) {
    auto target=graphics_surfaces_.find(destination),input=graphics_surfaces_.find(source);
    if(target==graphics_surfaces_.end() || input==graphics_surfaces_.end() || destination==source)return false;
    // Host accelerator for the original0040BDD0/0040BED0 copy,
    // 0040C5E0 constant opacity and modeC0 source fade. Geometry and
    // admitted format/mode dispatch above remain engine-owned.
    std::optional<PixelTransferOperation> gpu_operation;
    if (direct_copy &&
        target->second.bytes_per_pixel==4 && input->second.bytes_per_pixel==4)
        gpu_operation=PixelTransferOperation::Copy;
    else if(source_fade && parameter<=256 && target->second.format==1 && input->second.format==1)
        gpu_operation=PixelTransferOperation::FadeSource;
    else if((mode==1 || mode==0x20) && parameter<256 && target->second.format==1)
        gpu_operation=input->second.format==1?PixelTransferOperation::BlendOpaque:PixelTransferOperation::BlendAlpha;
    if(gpu_operation && materialize_graphics_pixels(target->second) && materialize_graphics_pixels(input->second)) {
        auto& out=target->second;auto& in=input->second;
        const int64_t left=std::max<int64_t>(x,0),top=std::max<int64_t>(y,0);
        const int64_t right=std::min<int64_t>(int64_t(x)+in.width,out.width);
        const int64_t bottom=std::min<int64_t>(int64_t(y)+in.height,out.height);
        if(left<right && top<bottom) {
            const uint64_t d0=uint64_t(top)*out.stride+uint64_t(left)*4;
            const uint64_t s0=uint64_t(top-y)*in.stride+uint64_t(left-x)*4;
            const auto w=static_cast<uint32_t>(right-left),h=static_cast<uint32_t>(bottom-top);
            const auto resident=in.pixels.gpu_storage();
            const std::span<uint8_t> source_bytes=resident?std::span<uint8_t>{}:std::span<uint8_t>(in.pixels);
            if(d0<=UINT32_MAX && s0<=UINT32_MAX && compose_host_pixel_transfer({
                    {out.pixels,static_cast<uint32_t>(d0),out.stride,w,h,out.format,4},
                    {source_bytes,static_cast<uint32_t>(s0),in.stride,w,h,in.format,4},*gpu_operation,parameter,0,resident}).success) {
                refresh_bgra_mirror(out);
                if(debug_enabled())bitmap_blits_.push_back({destination,source,x,y,mode,parameter});
                display_dirty_=true;return true;
            }
        }
    }
    return false;
}
void VM::offload_bitmap_cpu_pixels() {
    if(retain_cpu_pixel_copies())return;
    bool complete=true;
    std::vector<PixelBuffer*> pending;
    std::vector<std::span<const uint8_t>> uploads;
    const auto buffer=[&](PixelBuffer& p){
        if(p.gpu_storage() || p.empty()) {if(!p.offload())complete=false;}
        else {pending.push_back(&p);uploads.push_back(p.upload_bytes());}
    };
    const auto surface=[&](GraphicsSurface& s) {buffer(s.pixels);buffer(s.rgba);buffer(s.encoded);};
    for(auto& [id,s]:graphics_surfaces_){(void)id;surface(s);}
    for(auto& [id,w]:window_objects_) {
        (void)id;buffer(w.background_plane);buffer(w.overlay_plane);
        buffer(w.text_saved_plane);buffer(w.composite_plane);
        for(auto& layer:w.image_layers)surface(layer.image);
    }
    if(!pending.empty()) {
        const auto storage=store_host_pixel_storage_batch(uploads);
        for(size_t n=0;n<pending.size();++n)
            if(n>=storage.size() || !pending[n]->adopt_gpu_storage(storage[n]))complete=false;
    }
    static bool warned=false;
    if(!complete && !warned) {
        warned=true;
        std::cerr<<"warning: GPU_BITMAP_STORAGE_UNAVAILABLE; retaining CPU pixels for safe fallback\n";
    }
}
std::array<uint64_t,2> VM::bitmap_pixel_storage_bytes() const {
    std::array<uint64_t,2> bytes{};
    const auto buffer=[&](const PixelBuffer& p){bytes[0]+=p.capacity();bytes[1]+=p.gpu_bytes();};
    const auto surface=[&](const GraphicsSurface& s){buffer(s.pixels);buffer(s.rgba);buffer(s.encoded);};
    for(const auto& [id,s]:graphics_surfaces_){(void)id;surface(s);}
    for(const auto& [id,w]:window_objects_) {
        (void)id;buffer(w.background_plane);buffer(w.overlay_plane);buffer(w.text_saved_plane);buffer(w.composite_plane);
        for(const auto& layer:w.image_layers)surface(layer.image);
    }
    return bytes;
}

}
