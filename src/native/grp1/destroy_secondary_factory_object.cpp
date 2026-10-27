// Grp1:64 and :71 have distinct contracts in both original engine profiles.
namespace bgiplayer {
namespace {
NativeSignal grp1_set_effector_enabled(VM& vm, Thread& thread) {
    const uint32_t enabled = thread.stack.pop();
    const uint32_t handle = thread.stack.pop();
    if (!vm.set_effector_enabled(handle, enabled)) {
        throw std::runtime_error("Grp1:0x64 invalid effector handle");
    }
    return NativeSignal::Continue;
}

NativeSignal grp1_destroy_secondary_factory_object(VM& vm, Thread& thread) {
    return destroy_factory_object(
        vm, thread, VM::FactoryObjectKind::Grp1Secondary, "Grp1:0x71");
}
}  // namespace
}  // namespace bgiplayer
