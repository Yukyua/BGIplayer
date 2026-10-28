#include "bgiplayer/vm.hpp"
#include "bgiplayer/display_easing.hpp"
#include "bgiplayer/display_recorded_curve.hpp"

#include <bit>
#include <cstdlib>
#include <iostream>

namespace bgiplayer {
bool VM::install_control_display_procedure(Thread& thread,
    const std::array<int32_t, 11>& core, const std::array<int32_t, 6>& wrapper) {
    const uint32_t object = static_cast<uint32_t>(core[0]);
    if (!object_exists(object) || core[7] == 0) return false;
    display_procedures_.push_back({object, wrapper});
    ControlDisplayProcedureState p;
    p.object = object; p.core_arguments = core; p.wrapper_parameters = wrapper;
    auto start = object_position(object).value_or(std::pair<int32_t,int32_t>{0,0});
    if (object == 0 && background_type_ == 4U) {
        if (const auto f = sprite_mask_configs_.find(0); f != sprite_mask_configs_.end())
            start = {static_cast<int32_t>(f->second.field1), static_cast<int32_t>(f->second.field2)};
    }
    p.start_x = start.first; p.start_y = start.second;
    p.target_x = core[4] == 0 ? p.start_x : core[1];
    p.target_y = core[4] == 0 ? p.start_y : core[2];
    p.frame_rate = core[6]; // Legacy diagnostics: original +30 duration.
    p.frame_count = core[7]; // Original +38 return-value frequency.
    p.input_enabled = static_cast<uint32_t>(core[9]);
    p.input_mask = static_cast<uint32_t>(core[10]);
    p.start_effect = object_effect_level(object);
    if (object_display_classes_.contains(object) &&
        object_display_classes_.at(object) == DisplayObjectClass::CDspObjSprite &&
        object_raw_field(object, 0x254U) == 1U)
        p.start_effect = object_raw_field(object, 0x250U); // 0042D580
    p.target_effect = static_cast<uint32_t>(core[5]);
    p.start_time = object_raw_field(object, 0xC0U) >> 16U; // 004208B0
    p.target_time = p.start_time;
    p.started_ms = monotonic_ms();
    p.duration_ms = core[6] == 0 ? 1U : static_cast<uint32_t>(core[6]);
    p.period_limit_ms = static_cast<uint32_t>(
        static_cast<int32_t>(static_cast<uint32_t>(core[8]) * 1000U) / core[7]);
    p.next_update_ms = p.started_ms + 1U; // CProcedure+8(1),00436D70
    // 004371A9 invokes the TARGET's vtable+4(1). The base implementation
    // 0041FF30 enables +14 and walks all linked children unconditionally.
    std::set<uint32_t> enabled;
    const auto enable = [&](const auto& self, uint32_t current) -> void {
        if (!enabled.insert(current).second) return;
        set_base_draw_enabled(current, 1);
        if (const auto children = group_children_.find(current); children != group_children_.end())
            for (uint32_t child : children->second) self(self, child);
    };
    enable(enable, object);
    control_display_procedures_.push_back(p);
    const std::size_t index = control_display_procedures_.size() - 1U;
    if (debug_trace(DebugTrace::Display))
        std::cout << "DISPLAY_CONTROL_CREATE object=" << object << " effect=" << p.start_effect << ',' << p.target_effect
                  << " position=" << p.start_x << ',' << p.start_y << ',' << p.target_x << ',' << p.target_y
                  << " duration=" << p.duration_ms << " frequency=" << core[7] << " easing=" << core[3] << '\n';
    thread.snapshot_wait_kind = SnapshotWaitKind::DisplayControl;
    thread.snapshot_wait_index = index;
    thread.wait_procedure = [this,index] { return advance_control_display_procedure(index, monotonic_ms()); };
    thread.status_flags |= 1U;
    thread.state = ThreadState::Waiting;
    return true;
}

bool VM::install_framed_control_display_procedure(Thread& thread,
    const std::array<uint32_t,12>& a) {
    // 0049D950 passes a[1..9] straight to00437110; no time getter
    // normalization and no hardcoded effect-easing selector occur here.
    const std::array<int32_t,11> core{static_cast<int32_t>(a[0]),
        static_cast<int32_t>(a[1]),static_cast<int32_t>(a[2]),static_cast<int32_t>(a[3]),1,
        static_cast<int32_t>(a[4]),static_cast<int32_t>(a[7]),static_cast<int32_t>(a[8]),
        static_cast<int32_t>(a[9]),static_cast<int32_t>(a[10]),static_cast<int32_t>(a[11])};
    if (!install_control_display_procedure(thread,core,{})) return false;
    auto& p=control_display_procedures_.back();
    p.effect_easing=a[5];
    p.target_time=static_cast<int32_t>(a[6])<0 ? p.start_time : a[6];
    return true;
}

bool VM::install_recorded_linear_display_procedure(Thread& thread,
    const std::array<uint32_t,12>& a) {
    const auto& record=framed_control_procedures_.back().records;
    if (record.empty()) return false;
    auto simple=a;
    simple[1]=record.back()[0]; simple[2]=record.back()[1]; simple[5]&=0xFFFFU;
    if (!install_framed_control_display_procedure(thread,simple)) return false;
    auto& p=control_display_procedures_.back();
    p.absolute_coordinates=true;
    p.absolute_start=object_absolute_position(a[0]).value_or(std::array<uint32_t,3>{});
    p.absolute_target={record.back()[0],record.back()[1],record.back()[2]};
    // Existing serialized wrapper metadata is unused for this factory. Retain
    // an index+1 into the append-only record store, not a vector element pointer.
    // Old one-record snapshots with zero metadata keep their two-point path.
    p.wrapper_parameters[0]=static_cast<int32_t>(framed_control_procedures_.size());
    p.wrapper_parameters[1]=static_cast<int32_t>(a[5]>>16U);
    // 140071220 builds the path from the initial absolute XYZ and record XYZ;
    // record word3 is retained for the endpoint metadata but not a knot weight.
    p.start_x=static_cast<int32_t>(p.absolute_start[0]);
    p.start_y=static_cast<int32_t>(p.absolute_start[1]);
    p.target_x=static_cast<int32_t>(p.absolute_target[0]);
    p.target_y=static_cast<int32_t>(p.absolute_target[1]);
    return true;
}

WaitProcedureResult VM::advance_control_display_procedure(std::size_t index, uint64_t now) {
    auto& p = control_display_procedures_.at(index);
    if (p.complete) return {1, {p.reported_elapsed, 0U}};
    if (!object_exists(p.object)) throw BpFault("display control lost its target object");
    // CProcCtrlDspObj drains its current wait FIFO before the timer gate:
    // Mini32 00437EE0/00438080, legacy32 00437290/00437430.
    // A forced finish writes the exact target; cancellation exits without
    // another interpolation tick and retains the last rendered position.
    bool force = false;
    auto& thread = current_execution_thread();
    while (!thread.triple_command_queue.empty()) {
        const auto message = thread.triple_command_queue.front();
        thread.triple_command_queue.pop_front();
        if (message[0] == 0U) {
            p.reported_elapsed = static_cast<uint32_t>(
                static_cast<int32_t>(p.tick_count * 1000U) /
                static_cast<int32_t>(p.duration_ms));
            p.complete = true;
            return {1, {p.reported_elapsed, 0xFFFFFFFFU}};
        }
        if (message[0] == 1U && (message[1] != 0U || p.input_enabled != 0U))
            force = true;
    }
    if (!force && now < p.next_update_ms) return {0, {}};
    const uint64_t elapsed = now > p.started_ms ? now - p.started_ms : 0;
    // 004374E0 compares the duration as signed, so a negative duration
    // reaches the exact target immediately; zero was normalized at creation.
    uint32_t sample = static_cast<uint32_t>(elapsed);
    if (p.core_arguments[8] != 0) {
        const uint32_t interval = static_cast<uint32_t>(
            static_cast<int32_t>(static_cast<uint32_t>(p.core_arguments[8]) * 1000U) /
            p.core_arguments[7]);
        sample = std::min(sample, p.period_limit_ms);
        p.period_limit_ms = sample + interval;
    }
    const bool done = force || static_cast<int32_t>(p.duration_ms) <= static_cast<int32_t>(sample);
    const uint64_t bounded = done ? p.duration_ms : sample;
    const int32_t phase=done?0x1000000:static_cast<int32_t>((bounded*0x1000000U)/p.duration_ms);
    const int32_t linear=phase/256;
    const auto position_factor=evaluate_display_easing(phase,static_cast<uint32_t>(p.core_arguments[3]));
    if (!position_factor && (p.start_x!=p.target_x || p.start_y!=p.target_y ||
        (p.absolute_coordinates && (p.absolute_start[2]!=p.absolute_target[2] || p.wrapper_parameters[0]!=0))) && !done)
        throw BpFault("display control position easing selector is unresolved: object="+std::to_string(p.object)+" selector="+std::to_string(p.core_arguments[3]));
    int32_t effect_phase=phase;
    if(p.absolute_coordinates && p.wrapper_parameters[0]!=0 && !done) {
        // 140071098..10CB: independent effect clock, 2^32/(packed high16
        // or65536), then signed64 elapsed*factor<<8/duration, capped atQ24 one.
        const uint32_t divisor=p.wrapper_parameters[1]==0?65536U:static_cast<uint32_t>(p.wrapper_parameters[1]);
        const uint64_t factor=0x100000000ULL/divisor;
        const uint64_t product=(bounded*factor)<<8U;
        const int64_t effect_clock=std::bit_cast<int64_t>(product)/static_cast<int32_t>(p.duration_ms);
        effect_phase=static_cast<int32_t>(std::min<int64_t>(0x1000000,effect_clock));
    }
    const auto effect_factor=evaluate_display_easing(effect_phase,p.effect_easing);
    if (!effect_factor && p.start_effect!=p.target_effect && !done)
        throw BpFault("display control effect easing selector is unresolved: object="+std::to_string(p.object)+" selector="+std::to_string(p.effect_easing));
    const int64_t progress=done?65536:position_factor.value_or(linear);
    const int64_t effect_progress=done?65536:effect_factor.value_or(effect_phase/256);
    const auto interpolate = [done](int32_t start, int32_t target, int64_t factor) {
        if (done) return target;
        const int64_t delta = static_cast<int32_t>(static_cast<uint32_t>(target) - static_cast<uint32_t>(start));
        return static_cast<int32_t>(static_cast<uint32_t>(start) + static_cast<uint32_t>((delta * factor) >> 16));
    };
    int32_t x = interpolate(p.start_x,p.target_x,progress), y = interpolate(p.start_y,p.target_y,progress);
    std::array<uint32_t,3> absolute{};
    if (p.absolute_coordinates) {
        if(p.wrapper_parameters[0]!=0 && !done) {
            const size_t record_index=static_cast<uint32_t>(p.wrapper_parameters[0])-1U;
            if(record_index>=framed_control_procedures_.size())throw BpFault("recorded display control record index is invalid");
            const auto& records=framed_control_procedures_[record_index].records;
            std::vector<std::array<uint32_t,3>> points;
            points.reserve(std::min<size_t>(100,records.size()+1));
            points.push_back(p.absolute_start);
            for(const auto& record:records) {
                if(points.size()==100)break; // original append method capacity
                points.push_back({record[0],record[1],record[2]});
            }
            const auto sampled=evaluate_recorded_display_curve(points,static_cast<uint32_t>(progress));
            if(!sampled)throw BpFault("recorded display control path is empty");
            absolute=*sampled;
        } else {
            for (std::size_t axis=0;axis<3;++axis) {
                const int32_t first=static_cast<int32_t>(p.absolute_start[axis]);
                const int32_t delta=static_cast<int32_t>(p.absolute_target[axis]-p.absolute_start[axis]);
                absolute[axis]=done?p.absolute_target[axis]:static_cast<uint32_t>(first)+
                    static_cast<uint32_t>(static_cast<int32_t>(static_cast<double>(delta)*
                        (static_cast<double>(progress)/65536.0)));
            }
        }
        x=static_cast<int32_t>(absolute[0]); y=static_cast<int32_t>(absolute[1]);
    }
    const uint32_t effect = static_cast<uint32_t>(interpolate(static_cast<int32_t>(p.start_effect),
                                                             static_cast<int32_t>(p.target_effect),effect_progress));
    // 004374E0 uses elapsed/duration for the independent time field,
    // with a wrapped32-bit product before scaling it toQ16.
    const int32_t delta_time=static_cast<int32_t>(p.target_time-p.start_time);
    const int32_t time_product=static_cast<int32_t>(static_cast<uint32_t>(delta_time)*static_cast<uint32_t>(bounded));
    const uint32_t time = done ? p.target_time << 16U :
        (p.start_time << 16U)+static_cast<uint32_t>((static_cast<int64_t>(time_product)*65536)/static_cast<int32_t>(p.duration_ms));
    if (x != p.last_x || y != p.last_y || effect != p.last_effect || time != p.last_time ||
        (p.absolute_coordinates && object_absolute_position(p.object).value_or(std::array<uint32_t,3>{})[2]!=absolute[2])) {
        p.last_x = x; p.last_y = y; p.last_effect = effect; p.last_time = time;
        if (p.absolute_coordinates) {
            set_object_absolute_position(p.object,absolute[0],absolute[1],absolute[2]);
        } else if (p.object == 0 && background_type_ == 4U) {
            if (auto f = sprite_mask_configs_.find(0); f != sprite_mask_configs_.end()) {
                f->second.field1 = static_cast<uint32_t>(x); f->second.field2 = static_cast<uint32_t>(y);
            }
            object_positions_[0] = {x,y};
        } else set_object_position(p.object,x,y);
        // 004374E0 dispatches effect through virtual+48, not the coordinate
        // wrapper. Sprite0042D4D0 separates source crossfade from base opacity.
        std::set<uint32_t> visited;
        const auto apply = [&](const auto& self, uint32_t current, bool apply_effect) -> void {
            if (!visited.insert(current).second) return;
            const bool sprite = object_display_classes_.contains(current) &&
                object_display_classes_.at(current) == DisplayObjectClass::CDspObjSprite;
            const uint32_t selector = sprite ? object_raw_field(current, 0x254U) : 0xFFFFFFFFU;
            const bool base_effect = !sprite || selector == 0U || selector == 2U ||
                selector == 3U || selector == 0xFFFFFFFFU;
            if (apply_effect && base_effect) {
                object_raw_fields_[{current,0xB4U}] = effect;
                object_effect_level_[current] = effect;
                if (auto s = sprites_.find(current); s != sprites_.end()) s->second.alpha = effect;
                if (auto w = window_objects_.find(current); w != window_objects_.end()) w->second.transparency = effect;
            }
            if (apply_effect && sprite && (selector == 1U || selector == 2U))
                object_raw_fields_[{current,0x250U}] = effect;
            // +50(1,time) stores the already-Q16 word. Modes1/5/6 with
            // selector3 derive their source crossfade from its integer part.
            object_raw_fields_[{current,0xC0U}] = time;
            object_scaled_level_[current] = time;
            refresh_effector_parameters(current);
            const uint32_t mode = sprite ? object_raw_field(current, 0x144U) : 0U;
            if (sprite && selector == 3U && (mode == 1U || mode == 5U || mode == 6U))
                object_raw_fields_[{current,0x250U}] = time >> 16U;
            refresh_sprite_display_geometry(current);
            refresh_window_display_geometry(current);
            if (const auto children = group_children_.find(current); children != group_children_.end())
                for (uint32_t child : children->second) self(self,child,apply_effect && base_effect);
        };
        apply(apply,p.object,true);
        if (p.object == 0) background_effect_level_ = effect;
        display_dirty_ = true;
    }
    ++p.tick_count;
    p.reported_elapsed = static_cast<uint32_t>(
        static_cast<int32_t>(p.tick_count * 1000U) / static_cast<int32_t>(p.duration_ms));
    p.next_update_ms = now + 1U;
    if (!done) return {0,{}};
    p.complete = true;
    return {1,{p.reported_elapsed,force ? 1U : 0U}};
}
} // namespace bgiplayer
