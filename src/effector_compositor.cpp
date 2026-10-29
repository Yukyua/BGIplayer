#include "bgiplayer/vm.hpp"
#include "bgiplayer/gpu_pixel_kernels.hpp"
#include "bgiplayer/effector_selectors.hpp"
#include "bgiplayer/effector_displacement.hpp"
#include "bgiplayer/effector_vector.hpp"
#include "bgiplayer/effector_transform.hpp"
#include "bgiplayer/effector_temporal.hpp"
#include <chrono>
#include <iostream>
namespace bgiplayer {
void VM::refresh_effector_display_geometry(uint32_t handle){
    if(!effector_exists(handle))return;
    if(!display_object_states_.contains(handle))register_display_object(handle,0,handle);
    display_object_states_.at(handle).evaluate_priority=true;
    // Mini32 4264A0 / PE64 5B5A0: zero-origin primary canvas dimensions.
    const auto [width,height]=current_mode_dimensions();
    update_display_object_geometry(handle,0,0,width,height,object_draw_visible(handle));
}
bool VM::compose_effector_at(uint32_t destination,uint32_t handle,uint32_t left,uint32_t top){
    const auto fail=[&](std::string_view reason){return fail_display_draw(destination,handle,"effector",reason);};
    if(!effector_exists(handle))return fail("effector object is missing");
    if(!object_draw_visible(handle))return true;
    const auto effect=effector_effects_.find(handle);auto target=graphics_surfaces_.find(destination);
    if(effect==effector_effects_.end())return fail("effector state is missing");
    if(target==graphics_surfaces_.end())return fail("effector destination bitmap is missing");
    auto& state=effect->second;auto& out=target->second;
    if((out.format!=1&&out.format!=2)||out.bytes_per_pixel!=4)return true;
    if(!materialize_graphics_pixels(out))return fail("effector pixels could not be materialized");
    const auto [width,height]=current_mode_dimensions();auto& cache=state.temporal_cache;
    // 5B5A0 recreates the owned primary-sized cache on a geometry/format
    // change. Every setter retains it except69, which explicitly clears it.
    if(cache.width!=width||cache.height!=height||cache.format!=out.format||cache.bytes_per_pixel!=4) {
        cache=GraphicsSurface{};cache.width=width;cache.height=height;cache.stride=width*4;
        cache.format=out.format;cache.bytes_per_pixel=4;cache.defer_rgba_mirror=true;
        cache.pixels.assign(uint64_t(cache.stride)*cache.height,0);
    }
    if(out.width>width||out.height>height)return fail("effector callback descriptor exceeds its primary cache");
    const uint32_t threshold=object_visibility_threshold(handle),gate=object_raw_field(handle,0xBCU);
    uint32_t weight=object_effect_level(handle);
    switch(object_raw_field(handle,0xB0U)){
        case 1:case 0x20:case 0x21:case 0x22:case 0x23:case 0x24:
            weight=256U-(((256U-threshold)*(256U-weight)*gate)>>16U);break;
        case 2:case 3:case 4:case 0xc0:case 0xc1:
            weight=((256U-threshold)*weight*gate)>>16U;break;
        default:break;
    }
    // 5BD91..5BDA8 runs before dispatch/status validation, including zero or
    // out-of-range strength and missing/stale map tokens. Preserve this input
    // snapshot for subsequent temporal callbacks, and for single-axis blur.
    if(state.mode==1 && !retain_cpu_pixel_copies() && out.width==width && out.height==height)
        (void)out.pixels.offload();
    if(state.mode!=4) {
        if(out.width==width&&out.height==height&&out.stride==cache.stride)cache.pixels=out.pixels;
        else for(uint32_t y=0;y<out.height;++y)
            std::copy_n(out.pixels.data()+uint64_t(y)*out.stride,uint64_t(out.width)*4,
                        cache.pixels.data()+uint64_t(y)*cache.stride);
    }
    const bool trace_cost=debug_trace(DebugTrace::GpuFrames);
    const auto kernel_start=trace_cost?std::chrono::steady_clock::now():std::chrono::steady_clock::time_point{};
    // The immutable input is also the history snapshot. Keep it on the GPU
    // until a CPU fallback actually requests pixels.
    if(state.mode==1 && weight>0 && weight<=256 && state.mode_value<=5 &&
            out.width==width && out.height==height && !retain_cpu_pixel_copies() &&
            out.pixels.gpu_storage() && cache.pixels.gpu_storage()) {
        EffectorBlurPixels request{{{},0,out.stride,width,height,out.format,4},
            {{},0,cache.stride,width,height,cache.format,4},weight,state.mode_value,
            cache.pixels.gpu_storage()};
        if(auto result=compose_host_resident_effector_blur(request,out.pixels.gpu_storage())) {
            out.pixels.replace_gpu_storage(std::move(result));cache.rgba.clear();refresh_bgra_mirror(out);
            if(trace_cost)std::cout<<"GPU_EFFECTOR_KERNEL_COST object="<<handle<<" mode="<<state.mode
                <<" total_ns="<<std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-kernel_start).count()<<'\n';
            return true;
        }
    }
    const GlyphBitmapView view{out.pixels,0,out.stride,out.width,out.height,out.format,4};
    const auto& cached_pixels=std::as_const(cache.pixels);
    GlyphBitmapView input{{const_cast<uint8_t*>(cached_pixels.data()),cached_pixels.size()},0,cache.stride,width,height,cache.format,4};
    if(state.mode==1) {
        // 4B1A0 ignores the dirty rectangle. A cropped input descriptor does
        // not match cache dimensions, and the ignored status leaves pixels.
        if(weight>256||state.mode_value>5||out.width!=width||out.height!=height||weight==0)return true;
        if(!compose_host_effector_blur({view,input,weight,state.mode_value}).success &&
           !blur_pe64_effector_selector(view,input,weight,state.mode_value))
            return fail("effector blur rejected its pixel storage");
    } else if(state.mode==0) {
        if(weight>256)return true;
        // The zero-strength single-map copy otherwise traverses the full
        // cache beyond a cropped destination. Its visible crop already
        // equals the incoming scene; omit only the unsafe out-of-view write.
        if(state.secondary_bitmap==0xffffffffU && weight==0 &&
                (out.width!=input.width || out.height!=input.height))return true;
        const auto first=graphics_surfaces_.find(state.primary_bitmap);
        if(first==graphics_surfaces_.end()||graphics_bitmap_token(state.primary_bitmap)!=state.primary_token)return true;
        auto second=graphics_surfaces_.end();
        if(state.secondary_bitmap!=0xFFFFFFFFU) {
            second=graphics_surfaces_.find(state.secondary_bitmap);
            if(second==graphics_surfaces_.end()||graphics_bitmap_token(state.secondary_bitmap)!=state.secondary_token)return true;
        }
        auto& a=first->second;
        if(a.format!=4||a.width<out.width||a.height<out.height)return true;
        if(!materialize_graphics_pixels(a))return fail("effector displacement map could not be materialized");
        const GlyphBitmapView av{a.pixels,0,a.stride,a.width,a.height,a.format,a.bytes_per_pixel};
        std::optional<GlyphBitmapView> bv;
        if(second!=graphics_surfaces_.end()) {
            auto& b=second->second;
            if(b.format!=4||b.width!=a.width||b.height!=a.height)return true;
            if(!materialize_graphics_pixels(b))return fail("effector secondary displacement map could not be materialized");
            bv=GlyphBitmapView{b.pixels,0,b.stride,b.width,b.height,b.format,b.bytes_per_pixel};
        }
        if(!compose_host_effector_displacement({view,input,av,bv,weight,state.field_158!=0}).success &&
           !displace_pe64_effector(view,input,av,bv?&*bv:nullptr,weight,state.field_158))
            return fail("effector displacement rejected its pixel storage");
    } else if(state.mode==2) {
        const auto normal=graphics_surfaces_.find(state.vector_bitmap);
        if(normal==graphics_surfaces_.end()||graphics_bitmap_token(state.vector_bitmap)!=state.vector_token)return true;
        auto& map=normal->second;
        if(map.format!=6||map.width!=out.width||map.height!=out.height)return true;
        if(!materialize_graphics_pixels(map))return fail("effector normal map could not be materialized");
        std::vector<uint32_t> coefficients;coefficients.reserve(state.vector_records.size()*4);
        for(const auto& record:state.vector_records)coefficients.insert(coefficients.end(),record.begin(),record.end());
        const GlyphBitmapView normals{map.pixels,0,map.stride,map.width,map.height,6,map.bytes_per_pixel};
        const bool rectangular_bounds=display_constant_80111600_==1U;
        if(!compose_host_effector_vector({view,input,normals,coefficients,state.vector_bilinear!=0,rectangular_bounds}).success &&
           !vector_pe64_effector(view,input,normals,coefficients,state.vector_bilinear,rectangular_bounds))
            return fail("effector normal-map coefficient index or storage is invalid");
    } else if(state.mode==3) {
        const auto& transform=state.custom_transform;
        if(!transform[3] || !transform[4])return true;
        const auto matrix=effector_transform_matrix(cache.width,cache.height,transform);
        if((!matrix || !compose_host_effector_transform({view,input,*matrix,state.custom[6]!=0}).success) &&
           !transform_pe64_effector(view,input,transform,state.custom[6]))
            return fail("effector affine transform rejected its pixel storage");
    } else if(state.mode==4) {
        if(uint64_t(left)+out.width>cache.width||uint64_t(top)+out.height>cache.height)
            return fail("effector history crop is outside its primary cache");
        input.byte_offset=top*cache.stride+left*4;input.width=out.width;input.height=out.height;
        if(!compose_host_effector_temporal({view,input,weight}).success && !temporal_pe64_effector(view,input,weight))
            return fail("effector temporal blend rejected its pixel storage");
    } else return true; // Native callback's default dispatch is a no-op.
    if(trace_cost)std::cout<<"GPU_EFFECTOR_KERNEL_COST object="<<handle<<" mode="<<state.mode
        <<" total_ns="<<std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-kernel_start).count()<<'\n';
    cache.rgba.clear();refresh_bgra_mirror(out);return true;
}
}
