#include <stdexcept>

namespace bgiplayer {
uint32_t VM::capture_display_region_into_bitmap(uint32_t bitmap,
    int32_t x,int32_t y,uint32_t priority) {
    // PE64 140080650 resolves the existing bitmap before checking format1.
    // 14006F380 clears its storage and offsets the logical capture rectangle,
    // retaining the allocation token, format and dimensions.
    auto surface=graphics_surfaces_.find(bitmap);
    if(surface==graphics_surfaces_.end())return 2;
    if(surface->second.format!=1U)return 1;
    if(!materialize_graphics_pixels(surface->second))
        throw BpFault("display-region capture bitmap storage unavailable");
    std::fill(surface->second.pixels.begin(),surface->second.pixels.end(),0);
    refresh_bgra_mirror(surface->second);
    const auto neg=[](int32_t value){return static_cast<int32_t>(0U-static_cast<uint32_t>(value));};
    const std::array<int32_t,2> origin{neg(x),neg(y)};
    const std::array<int32_t,4> rectangle{origin[0],origin[1],
        static_cast<int32_t>(static_cast<uint32_t>(origin[0])+surface->second.width-1U),
        static_cast<int32_t>(static_cast<uint32_t>(origin[1])+surface->second.height-1U)};
    const auto callbacks=build_display_dispatch(bitmap,rectangle,0,
        (priority<<16U)|0xffffU,true,origin);
    if (debug_enabled()) display_bitmap_draws_.insert(display_bitmap_draws_.end(),callbacks.begin(),callbacks.end());
    for(const auto& callback:callbacks) {
        const bool executed=execute_display_callback(callback);
        if(display_callback_trace_)display_callback_trace_(callback,bitmap,executed);
        if(!executed) {
            std::ostringstream detail;
            detail<<"display-region capture callback is unsupported object=0x"<<std::hex
                <<callback.display_object<<" argument=0x"<<callback.dispatch_parameter
                <<" priority=0x"<<callback.object_priority<<std::dec
                <<" source="<<callback.source_left<<','<<callback.source_top<<','
                <<callback.source_right<<','<<callback.source_bottom
                <<" destination="<<callback.destination_left<<','<<callback.destination_top<<','
                <<callback.destination_right<<','<<callback.destination_bottom;
            throw BpFault(detail.str());
        }
    }
    return 0;
}
}
