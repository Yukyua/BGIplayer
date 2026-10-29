#include "bgiplayer/gpu_exact_pixel_ops.hpp"
#include "bgiplayer/pixel_buffer.hpp"
namespace bgiplayer {
bool compose_host_exact_pixel_buffers(ExactPixelPixels request,PixelBuffer& destination,
    const PixelBuffer* source,const PixelBuffer* second,bool cpu_result) {
    const auto op=static_cast<uint32_t>(request.operation);
    if((source==&destination && !(op>=1 && op<=6 && op!=2)) || second==&destination)return false;
    if(!exact_pixels_worthwhile(request.destination.width,request.destination.height,bool(destination.gpu_storage())))return false;
    const auto bind=[](GlyphBitmapView& view,std::shared_ptr<GpuPixelStorage>& storage,const PixelBuffer& bytes) {
        storage=bytes.gpu_storage();
        if(storage){view.pixels={};return;}
        const auto data=bytes.upload_bytes();
        view.pixels={const_cast<uint8_t*>(data.data()),data.size()};
    };
    bind(request.destination,request.destination_storage,destination);
    if(source)bind(request.source,request.source_storage,*source);
    if(second)bind(request.second,request.second_storage,*second);
    auto result=compose_host_resident_exact_pixels(request);
    if(!result)return false;
    if(cpu_result || retain_cpu_pixel_copies()) {
        std::vector<uint8_t> bytes(result->size());if(!result->read(bytes))return false;
        destination=std::move(bytes);
    } else destination.replace_gpu_storage(std::move(result));
    return true;
}
}
