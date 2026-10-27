// PE64 table140164F80[0F] ->1400E6310 ->14006C670.
namespace bgiplayer {
namespace {
NativeSignal grp2_set_scaled_font_spacing(VM& vm, Thread& thread) {
    const uint32_t spacing=thread.stack.pop();
    const uint32_t address=thread.stack.pop();
    const std::string name=vm.memory().read_cstring(address,thread.base);
    const auto status=vm.set_scaled_font_spacing(name,spacing);
    if(status==0x80000007U)throw std::runtime_error("Grp2:0x0F invalid letter-spacing adjustment");
    if(status==0x80000008U)throw std::runtime_error("Grp2:0x0F unknown font tuple: "+name);
    return NativeSignal::Continue;
}
}
}
