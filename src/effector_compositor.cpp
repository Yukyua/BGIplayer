#include "bgiplayer/vm.hpp"
#include "bgiplayer/effector_blur.hpp"
namespace bgiplayer {
void VM::refresh_effector_display_geometry(uint32_t handle){
    if(!extended_effector_modes()||!effector_exists(handle))return;
    if(!display_object_states_.contains(handle))register_display_object(handle,0,handle);
    display_object_states_.at(handle).evaluate_priority=true;
    // Mini32 4264A0 / PE64 5B5A0 compare the primary descriptor,
    // setting the effect object's zero-origin full primary dimensions.
    const auto [width,height]=current_mode_dimensions();
    update_display_object_geometry(handle,0,0,width,height,object_draw_visible(handle));
}
bool VM::compose_effector_at(uint32_t destination,uint32_t handle){
    const auto fail = [&](std::string_view reason) {
        return fail_display_draw(destination, handle, "effector", reason);
    };
    if(!extended_effector_modes())return fail("effector pixels are not implemented for legacy32");
    if(!effector_exists(handle))return fail("effector object is missing");
    if(!object_draw_visible(handle))return true;
    const auto effect=effector_effects_.find(handle);
    auto target=graphics_surfaces_.find(destination);
    if(effect==effector_effects_.end())return fail("effector state is missing");
    if(target==graphics_surfaces_.end())return fail("effector destination bitmap is missing");
    // Mini32 425C60→414810 / PE64 5BD50→4B1A0 kind1 mode5. Other
    // kinds/selectors and partial dirty views require separate callbacks.
    if(effect->second.mode!=1||effect->second.mode_value!=5)
        return fail("effector kind or selector is not implemented");
    auto& out=target->second;
    const auto [width,height]=current_mode_dimensions();
    if(out.width!=width||out.height!=height)return fail("effector partial dirty view is not implemented");
    if(!materialize_graphics_pixels(out))return fail("effector pixels could not be materialized");
    const uint32_t threshold=object_visibility_threshold(handle),gate=object_raw_field(handle,0xBCU);
    const uint32_t effect_value=object_effect_level(handle);
    uint32_t weight=effect_value;
    switch(object_raw_field(handle,0xB0U)){
        case 1:case 0x20:case 0x21:case 0x22:case 0x23:case 0x24:
            weight=256U-(((256U-threshold)*(256U-effect_value)*gate)>>16U);break;
        case 2:case 3:case 4:case 0xc0:case 0xc1:
            weight=((256U-threshold)*effect_value*gate)>>16U;break;
        default:break;
    }
    // 499A0/493F0 return status3 on invalid strength; 5BD50 ignores
    // this return without writing the destination.
    if(weight>256)return true;
    auto source=out.pixels; // 5BDA8 first copies the incoming scene to cache190.
    if(!blur_pe64_effector({out.pixels,0,out.stride,out.width,out.height,out.format,out.bytes_per_pixel},
            {source,0,out.stride,out.width,out.height,out.format,out.bytes_per_pixel},weight))
        return fail("effector blur kernel rejected the pixel format or layout");
    refresh_bgra_mirror(out);
    return true;
}
}
