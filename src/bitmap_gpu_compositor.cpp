#include "bgiplayer/vm.hpp"
#include "bgiplayer/gpu_pixel_kernels.hpp"
#include "bgiplayer/pixel_region.hpp"
#include <iostream>
namespace bgiplayer {
bool VM::accelerate_bitmap_transfer(uint32_t destination,uint32_t source,int32_t x,int32_t y,
                                    uint32_t mode,uint32_t parameter,bool direct_copy,bool source_fade) {
    auto target=graphics_surfaces_.find(destination),input=graphics_surfaces_.find(source);
    if(target==graphics_surfaces_.end() || input==graphics_surfaces_.end() || (destination==source && (x!=0 || y!=0)))return false;
    // Keep the original 2->2 opacity kernel and pair/odd-tail phase, but
    // apply it only to the clipped rows instead of materializing a complete
    // 1080p destination for a small label. Published owners stay immutable.
    if(destination!=source && !retain_cpu_pixel_copies() && (mode==1U || mode==0x20U) &&
            parameter>0 && parameter<256 && target->second.format==2 && input->second.format==2 &&
            materialize_graphics_pixels(target->second) && materialize_graphics_pixels(input->second)) {
        auto& out=target->second;auto& in=input->second;
        const auto base=out.pixels.gpu_storage(),source_owner=in.pixels.gpu_storage();
        const int64_t left=std::max<int64_t>(x,0),top=std::max<int64_t>(y,0);
        const int64_t right=std::min<int64_t>(int64_t(x)+in.width,out.width);
        const int64_t bottom=std::min<int64_t>(int64_t(y)+in.height,out.height);
        if(left<right && top<bottom && uint64_t(right-left)*uint64_t(bottom-top)<=65536 &&
            out.pixels.size()>=65536U*4 && base && base->fast_ranges() &&
            (!source_owner || source_owner->fast_ranges()) && out.bytes_per_pixel==4 && in.bytes_per_pixel==4 &&
            out.width<=UINT32_MAX/4 && in.width<=UINT32_MAX/4 && out.stride>=out.width*4 && in.stride>=in.width*4 &&
            uint64_t(out.height)*out.stride<=out.pixels.size() && uint64_t(in.height)*in.stride<=in.pixels.size()) {
            const auto patch=[&]() -> std::shared_ptr<GpuPixelStorage> {
                const auto w=static_cast<uint32_t>(right-left),h=static_cast<uint32_t>(bottom-top);
                PixelBuffer destination_bytes;std::vector<uint8_t> source_bytes(size_t(w)*h*4);
                auto* bytes=destination_bytes.overwrite(source_bytes.size());
                const auto* cpu_source=source_owner?nullptr:std::as_const(in.pixels).data();
                const size_t d0=size_t(top)*out.stride+size_t(left)*4;
                const size_t s0=size_t(top-y)*in.stride+size_t(left-x)*4;
                for(uint32_t row=0;row<h;++row) {
                    if(!base->read_range(d0+size_t(row)*out.stride,{bytes+size_t(row)*w*4,w*4}))return {};
                    auto source_row=std::span<uint8_t>(source_bytes).subspan(size_t(row)*w*4,w*4);
                    if(source_owner) {if(!source_owner->read_range(s0+size_t(row)*in.stride,source_row))return {};}
                    else std::copy_n(cpu_source+s0+size_t(row)*in.stride,source_row.size(),source_row.data());
                }
                if(!blend_original_glyph_opacity({{bytes,source_bytes.size()},0,w*4,w,h,2,4},
                        {source_bytes,0,w*4,w,h,2,4},0,0,parameter) || !destination_bytes.offload())return {};
                return patched_pixel_storage(base,destination_bytes.gpu_storage(),d0,out.stride,w*4,h);
            }();
            if(patch) {
                out.pixels.replace_gpu_storage(patch);refresh_bgra_mirror(out);
                if(debug_enabled())bitmap_blits_.push_back({destination,source,x,y,mode,parameter});
                display_dirty_=true;return true;
            }
        }
    }
    // Host accelerator for the original0040BDD0/0040BED0 copy,
    // 0040C5E0 constant opacity and modeC0 source fade. Geometry and
    // admitted format/mode dispatch above remain engine-owned.
    std::optional<PixelTransferOperation> gpu_operation;
    if (direct_copy &&
        target->second.bytes_per_pixel==4 && input->second.bytes_per_pixel==4)
        gpu_operation=PixelTransferOperation::Copy;
    else if((mode==1 || mode==0x20) && parameter==0 && input->second.format==1 &&
        (target->second.format==1 || target->second.format==2))
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
            // A same-format full copy does not consume any destination byte.
            // Allocate the packed result directly from the resident source.
            if(*gpu_operation==PixelTransferOperation::Copy && out.format==in.format && d0==0 &&
                    w==out.width && h==out.height && out.stride==uint64_t(w)*4 &&
                    out.pixels.size()==uint64_t(out.stride)*h && in.pixels.offload()) {
                if(auto result=crop_host_pixel_storage(in.pixels.gpu_storage(),s0,in.stride,out.stride,h)) {
                    out.pixels.replace_gpu_storage(std::move(result));refresh_bgra_mirror(out);
                    if(debug_enabled())bitmap_blits_.push_back({destination,source,x,y,mode,parameter});
                    display_dirty_=true;return true;
                }
            }
            if(out.pixels.size()>=65536U*4U && out.pixels.offload() && in.pixels.offload()) {
                if(auto result=compose_host_resident_transfer({
                    {{},static_cast<uint32_t>(d0),out.stride,w,h,out.format,4},
                    {{},static_cast<uint32_t>(s0),in.stride,w,h,in.format,4},*gpu_operation,parameter,0,
                    in.pixels.gpu_storage()},out.pixels.gpu_storage())) {
                    out.pixels.replace_gpu_storage(std::move(result));
                    refresh_bgra_mirror(out);
                    if(debug_enabled())bitmap_blits_.push_back({destination,source,x,y,mode,parameter});
                    display_dirty_=true;return true;
                }
            }
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
    const bool trace=debug_trace(DebugTrace::GpuFrames);
    struct Upload {uint32_t id,field;size_t bytes;};
    std::vector<Upload> records;
    const auto buffer=[&](PixelBuffer& p,uint32_t id,uint32_t field){
        if(p.gpu_storage() || p.empty()) {if(!p.offload())complete=false;}
        else {pending.push_back(&p);uploads.push_back(p.upload_bytes());if(trace)records.push_back({id,field,p.size()});}
    };
    const auto surface=[&](GraphicsSurface& s,uint32_t id) {buffer(s.pixels,id,0);buffer(s.rgba,id,1);buffer(s.encoded,id,2);};
    for(auto& [id,s]:graphics_surfaces_)surface(s,id);
    for(auto& [id,w]:window_objects_) {
        buffer(w.background_plane,id,3);buffer(w.overlay_plane,id,4);
        buffer(w.text_saved_plane,id,5);buffer(w.composite_plane,id,6);
        for(auto& layer:w.image_layers)surface(layer.image,id);
    }
    for(auto& [id,e]:effector_effects_)surface(e.temporal_cache,id);
    if(!pending.empty()) {
        const auto storage=store_host_pixel_storage_batch(uploads);
        for(size_t n=0;n<pending.size();++n)
            if(n>=storage.size() || !pending[n]->adopt_gpu_storage(storage[n]))complete=false;
    }
    if(trace && !records.empty()) {
        std::cout<<"GPU_RESIDENCY_UPLOAD count="<<records.size();
        for(const auto& record:records)if(record.bytes>=65536)
            std::cout<<" buffer="<<record.id<<':'<<record.field<<':'<<record.bytes;
        std::cout<<'\n';
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
    for(const auto& [id,e]:effector_effects_) {(void)id;surface(e.temporal_cache);}
    return bytes;
}

}
