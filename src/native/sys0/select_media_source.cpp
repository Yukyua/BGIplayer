namespace bgiplayer {
namespace {
NativeSignal sys0_select_media_source(VM& vm,Thread& thread){
    MediaSourceRequest request;
    request.prompt=thread.stack.pop();
    const uint32_t message=thread.stack.pop();
    const uint32_t caption=thread.stack.pop();
    const uint32_t path=thread.stack.pop();
    if(path==0)throw BpFault("Sys0:0x3F source path must not be null");
    request.path=vm.memory().read_cstring(path,thread.base);
    if(message!=0)request.message=vm.memory().read_cstring(message,thread.base);
    if(caption!=0)request.caption=vm.memory().read_cstring(caption,thread.base);
    request.encoding=vm.text_encoding_mode()==1?TextEncoding::Utf8:TextEncoding::ShiftJis;
    thread.stack.push(vm.select_media_source(request));
    return NativeSignal::Continue;
}
}
}
