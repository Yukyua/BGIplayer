namespace bgiplayer {
namespace {
NativeSignal sys1_dispatch_parallel_script(VM& vm,Thread& thread){
    const uint32_t entry=thread.stack.pop();
    const uint32_t data=thread.stack.pop();
    const uint32_t code=thread.stack.pop();
    const uint32_t slots=thread.stack.pop();
    vm.dispatch_parallel_script(slots,code,data,entry);
    return NativeSignal::Continue;
}
}
}
