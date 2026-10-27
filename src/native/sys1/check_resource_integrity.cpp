namespace bgiplayer {
namespace {
NativeSignal sys1_check_resource_integrity(VM& vm,Thread& thread) {
    const uint32_t resource=thread.stack.pop();
    const uint32_t archive=thread.stack.pop();
    if(archive==0){thread.stack.push(9);return NativeSignal::Continue;}
    if(resource==0)throw BpFault("Sys1:0x34 resource must not be null");
    auto data=vm.read_resource_integrity_data(
        vm.memory().read_cstring(archive,thread.base),
        vm.memory().read_cstring(resource,thread.base));
    if(!data){thread.stack.push(1);return NativeSignal::Continue;}
    vm.install_resource_integrity_wait(thread,std::move(*data));
    return NativeSignal::ResumeCurrent;
}
}
}
