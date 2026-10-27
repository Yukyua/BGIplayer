namespace bgiplayer {
namespace {
NativeSignal grp1_capture_display_region(VM& vm,Thread& thread) {
    if(!vm.compact_64_opcodes())
        throw BpFault("Grp1:0x05 legacy display-region capture is unverified");
    // 1400E4180: pop priority,y,x,bitmap. The manager receives {-x,-y}.
    const uint32_t priority=thread.stack.pop();
    const int32_t y=static_cast<int32_t>(thread.stack.pop());
    const int32_t x=static_cast<int32_t>(thread.stack.pop());
    const uint32_t bitmap=thread.stack.pop();
    thread.stack.push(vm.capture_display_region_into_bitmap(bitmap,x,y,priority));
    return NativeSignal::Continue;
}
}
}
