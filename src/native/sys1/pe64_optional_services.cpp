#include <random>
namespace bgiplayer {
namespace {
// 1400EDE50: rand_s, signed bound, unsigned remainder of host entropy.
NativeSignal sys1_secure_random(VM&, Thread& thread) {
    const int32_t bound = static_cast<int32_t>(thread.stack.pop());
    uint32_t result = 0;
    if (bound) {
        std::random_device entropy;
        const uint32_t magnitude = bound > 0 ? uint32_t(bound) : 0U-uint32_t(bound);
        result = uint32_t(entropy()) % magnitude;
        if (bound < 0) result = 0U-result;
    }
    thread.stack.push(result);
    return NativeSignal::Continue;
}
NativeSignal sys1_set_window_resizable(VM& vm, Thread& thread) {
    vm.set_window_resizable(thread.stack.pop()); return NativeSignal::Continue;
}
NativeSignal sys1_set_window_sizing_limit(VM& vm, Thread& thread) {
    const int32_t height=static_cast<int32_t>(thread.stack.pop());
    const int32_t width=static_cast<int32_t>(thread.stack.pop());
    thread.stack.push(vm.set_window_sizing_limit(width,height)?1U:0U);
    return NativeSignal::Continue;
}
NativeSignal sys1_query_client_size(VM& vm, Thread& thread) {
    const uint32_t output=thread.stack.pop();
    vm.memory().write_u32(output,thread.base,vm.client_width());
    vm.memory().write_u32(output+4,thread.base,vm.client_height());
    return NativeSignal::Continue;
}
// The four original helpers themselves return 80000004 unconditionally.
// Pointer resolution is still performed, as in 1400F10A0.
NativeSignal sys1_optional_service_pp(VM& vm, Thread& thread) {
    for (int i=0;i<2;++i) {
        const uint32_t address = thread.stack.pop();
        if (address) (void)vm.memory().read_bytes(address,0,thread.base);
    }
    thread.stack.push(1);
    return NativeSignal::Continue;
}
NativeSignal sys1_optional_service_ip(VM& vm, Thread& thread) {
    (void)thread.stack.pop();
    const uint32_t address=thread.stack.pop();
    if (address) (void)vm.memory().read_bytes(address,0,thread.base);
    thread.stack.push(1);
    return NativeSignal::Continue;
}
} // namespace
} // namespace bgiplayer
