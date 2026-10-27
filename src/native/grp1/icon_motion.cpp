namespace bgiplayer { namespace {
NativeSignal grp1_create_motion_icon_processor(VM& vm,Thread& thread) {
    const uint32_t window=thread.stack.pop();
    thread.stack.push(vm.create_icon_input_processor(window,2));return NativeSignal::Continue;
}
NativeSignal grp1_configure_icon_motion(VM& vm,Thread& thread) {
    std::array<uint32_t,10> arguments{};
    for(std::size_t i=arguments.size();i>0;--i)arguments[i-1]=thread.stack.pop();
    const uint32_t id=thread.stack.pop();
    thread.stack.push(vm.configure_icon_motion(id,arguments,thread.base));return NativeSignal::Continue;
}
} }
