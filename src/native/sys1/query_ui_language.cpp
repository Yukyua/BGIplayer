namespace bgiplayer {
namespace {
NativeSignal sys1_query_ui_language(VM& vm, Thread& thread) {
    thread.stack.push(vm.ui_language());
    return NativeSignal::Continue;
}
}
}
