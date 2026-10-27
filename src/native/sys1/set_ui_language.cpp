namespace bgiplayer {
namespace {
NativeSignal sys1_set_ui_language(VM& vm, Thread& thread) {
    thread.stack.push(vm.set_ui_language(thread.stack.pop()));
    return NativeSignal::Continue;
}
}
}
