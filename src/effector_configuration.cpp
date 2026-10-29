#include "bgiplayer/vm.hpp"
#include "bgiplayer/display_easing.hpp"

namespace bgiplayer {
namespace {
// 3ED30: the wave position is measured in dwords; phase/count are four-dword
// records. The phase wraps within a row before the contiguous tail is tested.
std::optional<uint32_t> wave_start(const VM::WaveTableState& table,
                                   uint32_t phase,uint32_t count) {
    if(!table.active || !table.row_dwords)return std::nullopt;
    uint32_t start=table.field_1c-phase*4U;
    if(static_cast<int32_t>(start)<static_cast<int32_t>(table.field_20))
        start=table.field_20+table.row_dwords-
            ((table.field_20-start)%table.row_dwords);
    if(uint64_t(count)*4U>uint64_t(table.total_dwords)-std::min(start,table.total_dwords) ||
       uint64_t(start)+uint64_t(count)*4U>table.samples.size())return std::nullopt;
    return start;
}
}
void VM::finish_effector_configuration(uint32_t handle,uint32_t effect,uint32_t query) {
    // 5B800/5B7C0 and base+A8: setters preserve cache190 and unrelated kind
    // parameters. Manager84D90..851C0 dirties only visible objects and reinserts.
    set_raw_field_recursive(handle,0xB4U,effect);
    if(query<0x10000U)object_raw_fields_[{handle,0x1CU}]=query;
    auto& state=effector_effects_.at(handle);
    state.effect=effect;state.query=object_raw_field(handle,0x1CU);
    refresh_effector_display_geometry(handle);
    reinsert_display_object(handle);
    display_dirty_=display_dirty_ || object_draw_visible(handle);
}
bool VM::configure_effector_bitmaps(uint32_t handle,uint32_t primary,uint32_t secondary,
                                  uint32_t field_158,uint32_t effect,uint32_t query) {
    if(!effector_exists(handle))return false;
    const auto [width,height]=current_mode_dimensions();
    const auto valid=[&](uint32_t bitmap){const auto it=graphics_surfaces_.find(bitmap);
        return it!=graphics_surfaces_.end() && it->second.format==4 &&
            it->second.width>=width && it->second.height>=height;};
    if(!valid(primary) || (secondary!=0xFFFFFFFFU && !valid(secondary)))return false;
    auto& state=effector_effects_[handle];state.vector_records.clear();
    state.mode=0;state.primary_bitmap=primary;state.secondary_bitmap=secondary;
    state.primary_token=graphics_bitmap_token(primary);
    state.secondary_token=graphics_bitmap_token(secondary);state.field_158=field_158;
    finish_effector_configuration(handle,effect,query);return true;
}
bool VM::configure_effector_mode(uint32_t handle,uint32_t mode,uint32_t effect,uint32_t query) {
    if(!effector_exists(handle) || mode>effector_mode_limit())return false;
    auto& state=effector_effects_[handle];state.vector_records.clear();
    state.mode=1;state.mode_value=mode;finish_effector_configuration(handle,effect,query);return true;
}
bool VM::configure_effector_custom(uint32_t handle,const std::array<uint32_t,9>& values) {
    // 5B8E0 / legacy425720: x,y,angle,scaleX,scaleY,interpolation,effect,bias.
    if(!effector_exists(handle) || !values[4] || !values[5])return false;
    auto& state=effector_effects_[handle];state.vector_records.clear();state.mode=3;
    state.custom=values;state.custom_modulation={};
    object_scaled_level_[handle]=0;object_raw_fields_[{handle,0xC0U}]=0;
    finish_effector_configuration(handle,values[7],values[8]);return true;
}
uint32_t VM::configure_effector_vector_records(uint32_t handle,uint32_t bitmap,uint32_t count,
                                              uint32_t source,uint32_t parameter_5,uint32_t query,
                                              uint32_t base) {
    (void)base;
    if(!effector_exists(handle))return 0xFF;
    const auto it=graphics_surfaces_.find(bitmap);
    if(it==graphics_surfaces_.end())return 1;
    const auto [width,height]=current_mode_dimensions();
    if(it->second.format!=6 || it->second.width!=width || it->second.height!=height)return 3;
    if(!count)return 6;
    if(source>=wave_tables_.size() || !wave_tables_[source].active)return 7;
    if(!wave_start(wave_tables_[source],0,count))return 8;
    auto& state=effector_effects_[handle];state.mode=2;state.vector_bitmap=bitmap;
    state.vector_token=graphics_bitmap_token(bitmap);state.vector_count=count;
    state.vector_source=source;state.vector_parameter_5=parameter_5;state.vector_phase=0;
    state.vector_records.resize(count);
    finish_effector_configuration(handle,parameter_5,query);return 0;
}
uint32_t VM::update_effector_vector_phase(uint32_t handle,uint32_t phase,uint32_t effect) {
    auto& state=effector_effects_.at(handle);
    if(state.mode!=2)return 0;
    if(state.vector_source>=wave_tables_.size() || !wave_tables_[state.vector_source].active)return 7;
    if(!wave_start(wave_tables_[state.vector_source],phase,state.vector_count))return 8;
    state.vector_phase=phase;
    if(effect>256)effect=object_effect_level(handle);
    set_raw_field_recursive(handle,0xB4U,effect);return 0;
}
void VM::refresh_effector_parameters(uint32_t handle) {
    const auto it=effector_effects_.find(handle);if(it==effector_effects_.end())return;
    auto& state=it->second;const auto effect=object_effect_level(handle);state.effect=effect;
    if(state.mode==2) {
        if(state.vector_source>=wave_tables_.size())return;
        const auto& table=wave_tables_[state.vector_source];
        const auto start=wave_start(table,state.vector_phase,state.vector_count);if(!start)return;
        // 3EDB0 scales the signed low word, truncates32-bit multiplication,
        // then duplicates its low16 result into independentX/Y coefficients.
        for(size_t i=0;i<state.vector_records.size()*4U;++i) {
            const uint32_t product=uint32_t(int32_t(int16_t(table.samples[*start+i])))*effect;
            const uint32_t sample=(product>>8U)&0xFFFFU;
            state.vector_records[i/4U][i%4U]=sample|(sample<<16U);
        }
    } else if(state.mode==3) {
        // 5B300: retain signed64 shifts and32-bit field writes. The scale
        // bases are unsigned dwords widened before subtracting Q16 one.
        const int32_t phase=static_cast<int32_t>(object_scaled_level(handle));
        const auto& m=state.custom_modulation;auto& t=state.custom_transform;
        const auto add=[&](uint32_t base,int32_t delta){return static_cast<int32_t>(base+uint32_t((int64_t(delta)*phase)>>24));};
        t[0]=add(state.custom[1],m[0]);t[1]=add(state.custom[2],m[1]);
        const int32_t eased=evaluate_display_easing(phase,static_cast<uint32_t>(m[5])).value_or(phase/256);
        const int64_t angle=int64_t(static_cast<int32_t>(state.custom[3]))+((int64_t(m[2])*eased)>>16);
        t[2]=static_cast<int32_t>((angle*effect)>>8);
        t[3]=static_cast<int32_t>((((int64_t(state.custom[4])-65536)+((int64_t(m[3])*phase)>>24))*effect)>>8)+65536U;
        t[4]=static_cast<int32_t>((((int64_t(state.custom[5])-65536)+((int64_t(m[4])*phase)>>24))*effect)>>8)+65536U;
    }
}
bool VM::clear_effector_effect(uint32_t handle,uint32_t effect,uint32_t query) {
    if(!effector_exists(handle))return false;
    auto& state=effector_effects_[handle];state.vector_records.clear();state.mode=4;
    state.temporal_cache.pixels.assign(state.temporal_cache.pixels.size(),0);
    state.temporal_cache.rgba.clear();finish_effector_configuration(handle,effect,query);return true;
}
}
