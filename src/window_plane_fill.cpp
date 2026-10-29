#include "bgiplayer/vm.hpp"
#include "bgiplayer/gpu_exact_pixel_ops.hpp"
namespace bgiplayer {
namespace {
void clear_owned_plane(PixelBuffer& pixels) {
    const size_t bytes=pixels.size();
    if(!bytes)return;
    if(bytes<=UINT32_MAX && exact_pixels_worthwhile(static_cast<uint32_t>(bytes),1)) {
        ExactPixelPixels request{{{},0,static_cast<uint32_t>(bytes),static_cast<uint32_t>(bytes),1,3,1},
            {},{},ExactPixelOperation::Fill};
        request.destination_bytes=bytes;
        if(auto result=compose_host_resident_exact_pixels(request)) {
            if(retain_cpu_pixel_copies()) {
                std::vector<uint8_t> copy(bytes);
                if(result->read(copy)){pixels=std::move(copy);return;}
            } else {pixels.replace_gpu_storage(std::move(result));return;}
        }
    }
    // Complete replacement: the old immutable GPU allocation is irrelevant.
    std::fill_n(pixels.overwrite(bytes),bytes,uint8_t{0});
}
}
uint32_t VM::append_window_bitmap_records(
    uint32_t window, uint32_t count, uint32_t records,
    uint32_t base, bool extended_records) {
    if (count == 0 || count > 0x40U) {
        return 1;
    }
    const auto object = window_objects_.find(window);
    if (object == window_objects_.end()) {
        return 0xFF;
    }
    const uint32_t stride = extended_records ? 0x40U : 0x10U;
    // Original B5 compacts offsets 0/4/8 from each 0x40-byte record.
    const uint32_t x_offset = 0U;
    const uint32_t y_offset = 4U;
    const uint32_t bitmap_offset = 8U;
    clear_owned_plane(object->second.overlay_plane);
    object->second.field_18c = 1;
    object->second.overlay_level = 0;
    for (auto& image : object->second.image_layers) image.enabled = 0;
    auto& commands = window_bitmap_commands_[window];
    for (uint32_t index = 0; index < count; ++index) {
        const uint32_t record = records + index * stride;
        const int32_t x = static_cast<int32_t>(
            memory_.read_u32(record + x_offset, base));
        const int32_t y = static_cast<int32_t>(
            memory_.read_u32(record + y_offset, base));
        const uint32_t bitmap =
            memory_.read_u32(record + bitmap_offset, base);
        const auto surface = graphics_surfaces_.find(bitmap);
        if (surface == graphics_surfaces_.end()) {
            continue;
        }
        const bool written = draw_window_plane(window,x,y,bitmap,0U,0U,true);
        commands.push_back({
            bitmap,
            x,
            y,
            surface->second.width,
            surface->second.height,
            extended_records ? 0xFFFFFFFFU
                            : memory_.read_u32(record + 12U, base),
            -1,
            0U,
            0U,
            1U,
            written,
        });
    }
    display_dirty_ = true;
    return 0;
}
uint32_t VM::fill_window_background(uint32_t window, uint32_t color) {
    const auto object = window_objects_.find(window);
    if (object == window_objects_.end()) {
        return 0xFF;
    }
    WindowObject& value = object->second;
    if (value.width == 0 || value.height == 0) {
        return 1;
    }
    const uint32_t format = value.plane_format;
    const uint32_t bytes_per_pixel =
        bitmap_bytes_per_pixel(format);
    if (bytes_per_pixel == 0) {
        return 1;
    }
    const size_t count=static_cast<size_t>(value.width)*value.height;
    const size_t size=count*bytes_per_pixel;
    bool filled=false;
    const uint32_t packed=format==0?color&0xFFFFU:(format==1?color&0xFFFFFFU:(format==3?color&255U:color));
    if(format<=3 && value.width<=UINT32_MAX/bytes_per_pixel &&
            exact_pixels_worthwhile(value.width,value.height)) {
        ExactPixelPixels request{{{},0,value.width*bytes_per_pixel,value.width,value.height,format,bytes_per_pixel},
            {},{},ExactPixelOperation::Fill};
        request.parameters[0]=packed;request.destination_bytes=size;
        if(auto result=compose_host_resident_exact_pixels(request)) {
            if(retain_cpu_pixel_copies()) {
                std::vector<uint8_t> copy(size);
                if(result->read(copy)){value.background_plane=std::move(copy);filled=true;}
            } else {value.background_plane.replace_gpu_storage(std::move(result));filled=true;}
        }
    }
    if(!filled) {
        if(format<=3) {
            auto* pixels=value.background_plane.overwrite(size);
            if(packed==0)std::fill_n(pixels,size,uint8_t{0});
            else for(size_t i=0;i<count;++i)for(uint32_t byte=0;byte<bytes_per_pixel;++byte)
                pixels[i*bytes_per_pixel+byte]=static_cast<uint8_t>(packed>>(byte*8U));
        } else value.background_plane.resize(size,0);
    }
    value.background_fill_color = color;
    value.background_bitmap = -1;
    value.background_active = false;
    display_dirty_ = true;
    return 0;
}
bool VM::reset_window_composition(uint32_t window) {
    const auto object = window_objects_.find(window);
    if (object == window_objects_.end()) {
        return false;
    }
    clear_owned_plane(object->second.overlay_plane);
    for (auto& layer : object->second.image_layers) {
        layer.enabled = 0;
    }
    window_bitmap_commands_.erase(window);
    selection_icon_processors_.erase(window);
    (void)set_window_text_direction(
        window, object->second.text_boundary_mode_398);
    display_dirty_ = true;
    return true;
}
}
