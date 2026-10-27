namespace bgiplayer {
namespace {
NativeSignal sys1_query_ui_language_japanese(VM& vm, Thread& thread) {
    thread.stack.push(vm.ui_language_is_japanese());
    return NativeSignal::Continue;
}
}
}
