#include "bgiplayer/vm.hpp"

#include <iomanip>
#include <iostream>
#include <sstream>

namespace bgiplayer {
namespace {
std::string resource_basename(std::string_view value) {
    return std::string(value.substr(value.find_last_of("/\\:") + 1U));
}
}

bool VM::fail_display_draw(uint32_t destination, uint32_t object,
                          std::string_view renderer, std::string_view reason,
                          uint32_t branch) {
    // Count failures even if already reported, so the callback wrapper can
    // distinguish a diagnosed failure from a failure in an uninstrumented arm.
    ++display_failure_serial_;
    const uint32_t handle = object == main_background_handle ? 0U : object;
    uint32_t primary = object_raw_field(handle, 0x160U);
    if (const auto sprite = sprites_.find(handle); sprite != sprites_.end() &&
        object_raw_field(handle, 0x144U) == 0U) primary = sprite->second.bitmap;
    uint32_t secondary = object_raw_field(handle, 0x164U);
    uint32_t mask = 0xFFFFFFFFU;
    if (object == main_background_handle) {
        primary = static_cast<uint32_t>(background_bitmaps_[0]);
        secondary = static_cast<uint32_t>(background_bitmaps_[1]);
        mask = static_cast<uint32_t>(background_bitmaps_[2]);
    } else if (const auto filter = filter_states_.find(handle); filter != filter_states_.end()) {
        primary = filter->second.bitmap;
    } else if (const auto effect = effector_effects_.find(handle); effect != effector_effects_.end()) {
        primary = effect->second.primary_bitmap;
        secondary = effect->second.secondary_bitmap;
    }
    if (renderer == "icon") primary = branch;
    const auto config = sprite_mode_configurations_.find(handle);
    const uint32_t api = config == sprite_mode_configurations_.end() ? 0xFFFFFFFFU : config->second.mode;
    const auto format = [this](uint32_t id, bool scratch = false) {
        if (id == 0xFFFFFFFFU && !scratch) return 0xFFFFFFFFU;
        const auto found = graphics_surfaces_.find(id);
        return found == graphics_surfaces_.end() ? 0xFFFFFFFFU : found->second.format;
    };
    const uint32_t target = display_warning_context_ ? display_warning_context_->bitmap : destination;
    std::ostringstream key;
    key << renderer << '|' << reason << '|' << object << '|' << branch << '|'
        << compact_64_opcodes_ << '|' << object_raw_field(handle, 0x144U) << '|'
        << api << '|' << object_raw_field(handle, 0xB0U) << '|'
        << object_raw_field(handle, 0x298U) << '|'
        << primary << '|' << secondary << '|' << mask << '|'
        << format(primary) << '|' << format(secondary) << '|' << format(mask) << '|'
        << target << '|' << format(destination, true);
    if (const auto window = window_objects_.find(handle); window != window_objects_.end())
        key << '|' << window->second.plane_format;
    if (object == main_background_handle) {
        key << '|' << background_type_;
        for (const auto& layer : background_effect_slots_)
            key << '|' << layer.mode << '|' << layer.field_184 << '|' << layer.field_188
                << '|' << layer.field_18C << '|' << layer.field_1A4;
    }
    if (const auto filter = filter_states_.find(handle); filter != filter_states_.end())
        key << '|' << filter->second.field_148 << '|' << filter->second.active;
    if (const auto effect = effector_effects_.find(handle); effect != effector_effects_.end())
        key << '|' << effect->second.mode << '|' << effect->second.mode_value;
    // Deliberately omit changing animation phase/position/opacity/scale and
    // callback rectangles. Their first values are logged below, not per frame.
    if (!display_warning_keys_.insert(key.str()).second) return false;

    std::ostringstream out;
    out << "warning: DRAW_UNSUPPORTED renderer=" << renderer
        << " reason=" << std::quoted(std::string(reason))
        << " object=0x" << std::hex << object << " class=0x" << object_class_id(handle)
        << " profile=" << (compact_64_opcodes_ ? "PE64" : "legacy32")
        << " raw_mode=0x" << object_raw_field(handle, 0x144U)
        << " api_mode=0x" << api << " blend_mode=0x" << object_raw_field(handle, 0xB0U)
        << " branch=0x" << branch << " target=0x" << target
        << " source=0x" << primary << " second=0x" << secondary << " mask=0x" << mask;
    if (display_warning_context_) {
        const auto& c = *display_warning_context_;
        out << " priority=0x" << c.object_priority << " dispatch=0x" << c.dispatch_parameter
            << std::dec << " clip=" << c.source_left << ',' << c.source_top << ','
            << c.source_right << ',' << c.source_bottom << " destination="
            << c.destination_left << ',' << c.destination_top << ','
            << c.destination_right << ',' << c.destination_bottom;
    }
    out << std::dec << " angle_q16=" << static_cast<int32_t>(object_raw_field(handle, 0x2A4U))
        << " scale_q16=" << object_raw_field(handle, 0x2B4U) << ',' << object_raw_field(handle, 0x2B8U)
        << " origin_q16=" << static_cast<int32_t>(object_raw_field(handle, 0x29CU)) << ','
        << static_cast<int32_t>(object_raw_field(handle, 0x2A0U))
        << " fraction=" << object_raw_field(handle, 0x2BCU) << ',' << object_raw_field(handle, 0x2C0U)
        << " stretch=" << object_raw_field(handle, 0x318U) << ',' << object_raw_field(handle, 0x320U)
        << " filter=" << object_raw_field(handle, 0x298U)
        << " mix=" << object_raw_field(handle, 0x250U)
        << " effect=" << object_effect_level(handle)
        << " threshold=" << object_visibility_threshold(handle)
        << " gate=" << object_raw_field(handle, 0xBCU);
    uint32_t parameter = object_effect_level(handle);
    const uint32_t blend = object_raw_field(handle, 0xB0U);
    const uint32_t threshold = object_visibility_threshold(handle);
    const uint32_t gate = object_raw_field(handle, 0xBCU);
    if (blend == 1U || (blend >= 0x20U && blend <= 0x24U))
        parameter = 256U - (((256U-threshold)*(256U-parameter)*gate)>>16U);
    else if ((blend >= 2U && blend <= 4U) || blend == 0xC0U || blend == 0xC1U)
        parameter = ((256U-threshold)*parameter*gate)>>16U;
    out << " effective_parameter=" << parameter << " modulation={";
    for (uint32_t block = 0; block < 4U; ++block)
        for (uint32_t offset : {0x370U, 0x37CU, 0x388U, 0x394U}) {
            const auto field = offset + block * 0x30U;
            if (const auto value = object_raw_field(handle, field); value != 0U)
                out << "0x" << std::hex << field << ":0x" << value << ',';
        }
    out << "}";
    if (config != sprite_mode_configurations_.end()) {
        out << " operands=[";
        for (auto value : config->second.operands) out << "0x" << std::hex << value << ',';
        out << ']';
    }
    const auto layout = [&](std::string_view name, uint32_t id, bool scratch = false) {
        out << ' ' << name << "={";
        if (id == 0xFFFFFFFFU && !scratch) { out << "none}"; return; }
        const auto surface = graphics_surfaces_.find(id);
        if (surface == graphics_surfaces_.end()) out << "missing";
        else {
            const auto& s = surface->second;
            out << "format=" << std::dec << s.format << " size=" << s.width << 'x' << s.height
                << " stride=" << s.stride << " bpp=" << s.bytes_per_pixel
                << " pixels=" << s.pixels.size() << " encoded=" << s.encoded.size()
                << " token=" << s.allocation_token << " resource="
                << std::quoted(resource_basename(s.archive) + ":" + resource_basename(s.resource));
        }
        out << '}';
    };
    layout("source_layout", primary); layout("second_layout", secondary);
    layout("mask_layout", mask); layout("target_layout", destination, true);
    out << std::dec;
    if (const auto window = window_objects_.find(handle); window != window_objects_.end()) {
        const auto& w = window->second;
        out << " window={format=" << w.plane_format << " size=" << w.width << 'x' << w.height
            << " overlay=" << w.overlay_level << " isolated=" << w.isolated_composition << " order=";
        for (auto stage : w.compose_order) out << stage << ',';
        out << '}';
    }
    if (object == main_background_handle) {
        out << " background_type=" << background_type_ << " position_q16="
            << background_position_[0] << ',' << background_position_[1];
        if (const auto mask_config = sprite_mask_configs_.find(0); mask_config != sprite_mask_configs_.end()) {
            const auto& c = mask_config->second;
            out << " backf=" << c.field1 << ',' << c.field2 << ',' << c.field4 << ','
                << c.field5 << ',' << c.field8;
        }
        for (size_t i = 0; i < background_effect_slots_.size(); ++i) {
            const auto& s = background_effect_slots_[i];
            if (s.field_150 == 0 && s.field_154 == 0) continue;
            out << " layer" << i << "={bitmap=" << s.bitmap << " mode=" << s.mode
                << " parameter=" << s.field_164 << " fixed=" << s.fixed_x << ',' << s.fixed_y
                << " origin=" << s.field_170 << ',' << s.field_174 << " angle=" << s.field_178
                << " scale=" << s.field_17C << ',' << s.field_180 << " selectors="
                << s.field_184 << ',' << s.field_188 << ',' << s.field_18C
                << " option=" << s.field_1A4 << " velocity=" << s.field_190 << ',' << s.field_194
                << ',' << s.field_198 << ',' << s.field_19C << ',' << s.field_1A0 << '}';
            layout("layer_source", static_cast<uint32_t>(s.bitmap));
        }
    }
    if (const auto filter = filter_states_.find(handle); filter != filter_states_.end())
        out << " filter_mode=" << filter->second.field_148 << " color=" << filter->second.field_14c
            << " bitmap_mode=" << filter->second.active;
    if (const auto effect = effector_effects_.find(handle); effect != effector_effects_.end())
        out << " effector_mode=" << effect->second.mode << " selector=" << effect->second.mode_value;
    // Script resource names only: never expose the game's host directory.
    const uint32_t id = current_thread_id();
    std::string script;
    uint32_t ip = thread_.ip;
    if (const auto thread = program_threads_.find(id); thread != program_threads_.end()) {
        script = thread->second.source;
        ip = thread->second.thread.ip;
    } else if (const auto resource = resource_segments_.find(active_code_resource_base_);
               resource != resource_segments_.end()) script = resource->second.path;
    script = resource_basename(script);
    out << " thread=" << std::dec << id << " ip=0x" << std::hex << ip
        << " script=" << std::quoted(script) << "; draw failed (diagnostic only)\n";
    std::cerr << out.str();
    return false;
}

} // namespace bgiplayer
