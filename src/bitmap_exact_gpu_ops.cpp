#include "bgiplayer/vm.hpp"
#include "bgiplayer/gpu_exact_pixel_ops.hpp"
#include <cstdlib>
#include <cstring>
namespace bgiplayer {
bool VM::accelerate_exact_bitmap_blit(uint32_t destination,uint32_t source,int32_t x,int32_t y,uint32_t mode,uint32_t parameter) {
    auto target=graphics_surfaces_.find(destination),input=graphics_surfaces_.find(source);
    if(target==graphics_surfaces_.end()||input==graphics_surfaces_.end()||destination==source)return false;
    auto& out=target->second;auto& in=input->second;
    const uint32_t df=out.format,sf=in.format;
    const uint32_t original_mode=mode;
    if(sf==3&&df==2&&(mode==128||((mode==5||mode==192||mode==193||mode==240)&&parameter==0)))mode=128;
    if(mode==255&&parameter>=4&&sf==0&&df==0)mode=0;
    const char* gpu_mode=std::getenv("BGI_GPU");
    if(mode==65 && !(gpu_mode && std::strcmp(gpu_mode,"1")==0))return false;
    const bool rgb=sf==1||sf==2;
    const bool accepted=(mode==255&&parameter<4&&rgb&&out.bytes_per_pixel>=4) ||
        (mode==128&&sf==3&&df==2) || (mode==0&&sf==0&&df==0) || mode==65 ||
        (mode==64&&((sf==0&&df==0)||(rgb&&(df==1||df==2)))) ||
        (mode==193&&parameter>0&&parameter<256&&sf==df&&df<=2) ||
        (mode==240&&parameter>0&&parameter<256&&((sf==0&&df==0)||(rgb&&(df==1||df==2)))) ||
        ((mode==8||mode==9||mode==38||mode==39)&&parameter<=256&&rgb&&df==1) ||
        ((mode==7||mode==37)&&parameter<=256&&sf==2&&(df==1||df==2||df==3)) ||
        ((mode==6||mode==36)&&parameter<=256&&rgb&&df==1) ||
        ((mode==4||mode==35)&&parameter<=256&&((sf==1&&(df==1||df==2))||(sf==2&&df==1)||(sf==3&&df==3))) ||
        ((mode==3||mode==34)&&parameter<=256&&sf==2&&df==1) ||
        ((mode==2||mode==33)&&parameter<=256&&sf==0&&df==0);
    if(!accepted)return false;
    // Zero-strength arms are original no-ops. Let their CPU dispatch preserve
    // all diagnostics; no GPU command is useful for them.
    const uint32_t ep=mode>=33&&mode<=39?256-parameter:parameter;
    if((mode==6||mode==36||mode==4||mode==35||mode==3||mode==34||mode==2||mode==33)&&ep==0)return false;
    if((mode==2||mode==33)&&ep==256)return false;
    const int32_t placement_x=mode==65?0:x,placement_y=mode==65?0:y;
    const int64_t left=std::max<int64_t>(placement_x,0),top=std::max<int64_t>(placement_y,0);
    const int64_t right=std::min<int64_t>(int64_t(placement_x)+in.width,out.width),bottom=std::min<int64_t>(int64_t(placement_y)+in.height,out.height);
    if(left>=right||top>=bottom || !exact_pixels_worthwhile(static_cast<uint32_t>(right-left),static_cast<uint32_t>(bottom-top),bool(out.pixels.gpu_storage())))return false;
    if(!materialize_graphics_pixels(out)||!materialize_graphics_pixels(in))return false;
    if(!retain_cpu_pixel_copies())(void)in.pixels.offload();
    const uint64_t d0=uint64_t(top)*out.stride+uint64_t(left)*out.bytes_per_pixel;
    const uint64_t s0=uint64_t(top-placement_y)*in.stride+uint64_t(left-placement_x)*in.bytes_per_pixel;
    if(d0>UINT32_MAX||s0>UINT32_MAX)return false;
    const uint32_t w=static_cast<uint32_t>(right-left),h=static_cast<uint32_t>(bottom-top);
    const auto span=[](const PixelBuffer& bytes){if(bytes.gpu_storage())return std::span<uint8_t>{};const auto b=bytes.upload_bytes();return std::span<uint8_t>(const_cast<uint8_t*>(b.data()),b.size());};
    ExactPixelPixels p{{span(out.pixels),static_cast<uint32_t>(d0),out.stride,w,h,df,out.bytes_per_pixel},
        {span(in.pixels),static_cast<uint32_t>(s0),in.stride,w,h,sf,in.bytes_per_pixel},{},ExactPixelOperation::VmBlit};
    p.parameters[0]=mode;p.parameters[1]=parameter;
    p.destination_storage=out.pixels.gpu_storage();p.source_storage=in.pixels.gpu_storage();
    auto result=compose_host_resident_exact_pixels(p);
    if(!result)return false;
    std::shared_ptr<GpuPixelStorage> rgba_result;
    if(mode==64 || mode==65) {
        PixelBuffer mirror=out.rgba;
        const size_t count=size_t(out.width)*out.height*4;
        if(mirror.size()!=count)mirror.assign(count,0);
        auto rgba=p;
        rgba.operation=ExactPixelOperation::VmClearMirror;rgba.parameters[2]=df;
        rgba.destination={span(mirror),static_cast<uint32_t>(uint64_t(top)*out.width*4+uint64_t(left)*4),out.width*4,w,h,2,4};
        rgba.destination_storage=mirror.gpu_storage();
        if(rgba.destination_storage)rgba.destination.pixels={};
        rgba_result=compose_host_resident_exact_pixels(rgba);
        if(!rgba_result)return false;
    }
    std::vector<uint8_t> native_cpu,rgba_cpu;
    if(retain_cpu_pixel_copies()) {
        native_cpu.resize(result->size());if(!result->read(native_cpu))return false;
        if(rgba_result){rgba_cpu.resize(rgba_result->size());if(!rgba_result->read(rgba_cpu))return false;}
    }
    if(retain_cpu_pixel_copies())out.pixels=std::move(native_cpu);
    else out.pixels.replace_gpu_storage(std::move(result));
    if(rgba_result) {
        if(retain_cpu_pixel_copies())out.rgba=std::move(rgba_cpu);
        else out.rgba.replace_gpu_storage(std::move(rgba_result));
    } else if(df==1 || df==2)refresh_bgra_mirror(out);
    if(debug_enabled())bitmap_blits_.push_back({destination,source,x,y,original_mode,parameter});
    display_dirty_=true;return true;
}
}
