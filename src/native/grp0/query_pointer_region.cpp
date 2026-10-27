namespace bgiplayer {
namespace {
// PE64 1400DCFB0 -> 1400C52A0 / 1400C5300. TOS buttons*, priority*.
NativeSignal grp0_query_pointer_region(VM& vm, Thread& thread) {
    const uint32_t buttons_output=thread.stack.pop();
    const uint32_t priority_output=thread.stack.pop();
    uint32_t priority=0;
    if(vm.sys0_input_global_52a5ac()) {
        for(const auto& region:vm.input_regions()) {
            if(vm.input_region_hit(region.priority)) {priority=region.priority;break;}
        }
    }
    if(!priority) {thread.stack.push(0);return NativeSignal::Continue;}
    uint32_t buttons=0;
    for(const auto& pair:std::array<std::pair<uint32_t,uint32_t>,5>{{{1,1},{2,2},{4,4},{5,16},{6,32}}})
        if(vm.input_descriptor(pair.first)&0x8000U)buttons|=pair.second;
    vm.memory().write_u32(priority_output,thread.base,priority);
    vm.memory().write_u32(buttons_output,thread.base,buttons);
    thread.stack.push(1);return NativeSignal::Continue;
}
}
}
