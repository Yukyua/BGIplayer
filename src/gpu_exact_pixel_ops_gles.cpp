#ifdef __ANDROID__
#include "bgiplayer/gpu_exact_pixel_ops.hpp"
#include <EGL/egl.h>
#include <GLES3/gl31.h>
#include <cstring>
#include <iostream>
namespace bgiplayer::exact_pixel_detail {
namespace {
struct RestoreContext {
    EGLDisplay display=eglGetCurrentDisplay();EGLContext context=eglGetCurrentContext();
    EGLSurface draw=eglGetCurrentSurface(EGL_DRAW),read=eglGetCurrentSurface(EGL_READ);
    ~RestoreContext(){if(display!=EGL_NO_DISPLAY)eglMakeCurrent(display,draw,read,context);else{const auto current=eglGetCurrentDisplay();if(current!=EGL_NO_DISPLAY)eglMakeCurrent(current,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);}}
};
struct Device {
    EGLDisplay display=EGL_NO_DISPLAY;EGLContext context=EGL_NO_CONTEXT;EGLSurface surface=EGL_NO_SURFACE;GLuint program=0;
    std::shared_ptr<void> lifetime;std::recursive_mutex* mutex=nullptr;
    Device(){
        const auto native=host_native_pixel_context();
        if(!native.context || !native.mutex || !native.lifetime)return;
        lifetime=native.lifetime;mutex=native.mutex;
        display=(EGLDisplay)native.display;context=(EGLContext)native.context;surface=(EGLSurface)native.surface;
        std::lock_guard lock(*mutex);RestoreContext restore;if(!current())return;
        GLint glMajor=0,glMinor=0;glGetIntegerv(GL_MAJOR_VERSION,&glMajor);glGetIntegerv(GL_MINOR_VERSION,&glMinor);
        if(glMajor<3 || (glMajor==3 && glMinor<1))return;
        const std::string source=R"GLSL(#version 310 es
precision highp int;
layout(local_size_x=16,local_size_y=8) in;
layout(std430,binding=0) buffer Destination {uint d[];};
layout(std430,binding=1) readonly buffer Source {uint a[];};
layout(std430,binding=2) readonly buffer Second {uint b[];};
layout(std430,binding=3) readonly buffer Arguments {uint p[];};
uint byteA(uint i){return (a[i>>2u]>>((i&3u)*8u))&255u;}
uint byteB(uint i){return (b[i>>2u]>>((i&3u)*8u))&255u;}
uint byteD(uint i){return (atomicOr(d[i>>2u],0u)>>((i&3u)*8u))&255u;}
void writeD(uint i,uint v){uint shift=(i&3u)*8u;atomicAnd(d[i>>2u],~(255u<<shift));atomicOr(d[i>>2u],(v&255u)<<shift);}
#define A(i) byteA(uint(i))
#define B(i) byteB(uint(i))
#define D(i) byteD(uint(i))
#define W(i,v) writeD(uint(i),uint(v))
void main(){uint x=gl_GlobalInvocationID.x,y=gl_GlobalInvocationID.y;
)GLSL"+shader_body()+"\n}\n";
        GLuint shader=glCreateShader(GL_COMPUTE_SHADER);const char* ptr=source.c_str();glShaderSource(shader,1,&ptr,nullptr);glCompileShader(shader);
        GLint ok=0;glGetShaderiv(shader,GL_COMPILE_STATUS,&ok);
        if(!ok){char log[8192]{};glGetShaderInfoLog(shader,sizeof(log),nullptr,log);std::cerr<<"Exact GLES shader: "<<log<<'\n';glDeleteShader(shader);return;}
        program=glCreateProgram();glAttachShader(program,shader);glLinkProgram(program);glDeleteShader(shader);glGetProgramiv(program,GL_LINK_STATUS,&ok);
        if(!ok){glDeleteProgram(program);program=0;}
    }
    bool current(){return eglMakeCurrent(display,surface,surface,context)==EGL_TRUE;}
    ~Device(){if(!mutex)return;std::lock_guard lock(*mutex);RestoreContext restore;if(current()&&program)glDeleteProgram(program);}
};
std::shared_ptr<Device> device(){static thread_local auto d=std::make_shared<Device>();return d;}
class Storage final:public GpuPixelStorage {
public:
    std::shared_ptr<Device> device;GLuint buffer;size_t bytes;
    Storage(std::shared_ptr<Device> d,GLuint b,size_t n):device(std::move(d)),buffer(b),bytes(n){}
    ~Storage()override{std::lock_guard lock(*device->mutex);RestoreContext restore;if(device->current())glDeleteBuffers(1,&buffer);}
    const void* native_handle(std::string_view kind)const override {
        if(kind=="gles-buffer")return reinterpret_cast<const void*>(uintptr_t(buffer));
        if(kind=="gles-context")return device->context;
        if(kind=="gles-display")return device->display;
        return nullptr;
    }
    size_t size()const override{return bytes;}
    bool read(std::span<uint8_t> out)const override{return out.size()==bytes && read_range(0,out);}
    bool fast_ranges()const override{return true;}
    bool read_range(size_t at,std::span<uint8_t> out)const override{
        if(at>bytes || out.size()>bytes-at)return false;std::lock_guard lock(*device->mutex);RestoreContext restore;if(!device->current())return false;
        glBindBuffer(GL_COPY_READ_BUFFER,buffer);void* map=glMapBufferRange(GL_COPY_READ_BUFFER,static_cast<GLintptr>(at),static_cast<GLsizeiptr>(out.size()),GL_MAP_READ_BIT);
        if(!map)return false;std::vector<uint8_t> temporary(out.size());std::memcpy(temporary.data(),map,out.size());
        if(glUnmapBuffer(GL_COPY_READ_BUFFER)!=GL_TRUE)return false;std::copy(temporary.begin(),temporary.end(),out.begin());return true;
    }
};
GLuint upload(std::span<const uint8_t> bytes){GLuint b=0;glGenBuffers(1,&b);glBindBuffer(GL_SHADER_STORAGE_BUFFER,b);glBufferData(GL_SHADER_STORAGE_BUFFER,static_cast<GLsizeiptr>(std::max(size_t(4),(bytes.size()+3)&~size_t(3))),nullptr,GL_STATIC_DRAW);if(!bytes.empty())glBufferSubData(GL_SHADER_STORAGE_BUFFER,0,static_cast<GLsizeiptr>(bytes.size()),bytes.data());return b;}
std::shared_ptr<GpuPixelStorage> run(const ExactPixelPixels& p){
    auto d=device();if(!d->program)return {};std::lock_guard lock(*d->mutex);RestoreContext restore;if(!d->current())return {};
    // Keep owners from this context on the GPU. Other contexts cannot share
    // names; rejecting them preserves the caller's original CPU fallback.
    const auto native=[&](const std::shared_ptr<GpuPixelStorage>& owner)->GLuint{
        if(!owner || owner->native_handle("gles-context")!=d->context || owner->native_handle("gles-display")!=d->display)return 0;
        const auto value=reinterpret_cast<uintptr_t>(owner->native_handle("gles-buffer"));
        return value<=UINT32_MAX?static_cast<GLuint>(value):0;
    };
    GLuint initial=p.destination_storage?native(p.destination_storage):0,a=p.source_storage?native(p.source_storage):0,b=p.second_storage?native(p.second_storage):0;
    if((p.destination_storage&&!initial)||(p.source_storage&&!a)||(p.second_storage&&!b))return {};
    GLint maximum=0;glGetIntegerv(GL_MAX_SHADER_STORAGE_BLOCK_SIZE,&maximum);
    const auto native_size_ok=[&](GLuint name,size_t required){if(!name)return true;glBindBuffer(GL_COPY_READ_BUFFER,name);GLint64 capacity=0;glGetBufferParameteri64v(GL_COPY_READ_BUFFER,GL_BUFFER_SIZE,&capacity);return capacity>=0&&uint64_t(capacity)>=required;};
    const size_t outputBytes=p.destination_storage?p.destination_storage->size():(p.destination_bytes?p.destination_bytes:p.destination.pixels.size());
    if(maximum<=0 || outputBytes>size_t(maximum) ||
       (p.source_storage?p.source_storage->size():p.source.pixels.size())>size_t(maximum) ||
       (p.second_storage?p.second_storage->size():p.second.pixels.size())>size_t(maximum) ||
       !native_size_ok(initial,p.destination_storage?p.destination_storage->size():0) ||
       !native_size_ok(a,p.source_storage?p.source_storage->size():0) || !native_size_ok(b,p.second_storage?p.second_storage->size():0))return {};
    for(unsigned i=0;i<16 && glGetError()!=GL_NO_ERROR;++i){}
    bool ownInitial=!initial,ownA=!a,ownB=!b;
    if(ownInitial)initial=upload(p.destination.pixels);if(ownA)a=upload(p.source.pixels);if(ownB)b=upload(p.second.pixels);
    const size_t bytes=p.destination_storage?p.destination_storage->size():(p.destination_bytes?p.destination_bytes:p.destination.pixels.size());
    GLuint out=0,argsBuffer=0;glGenBuffers(1,&out);glBindBuffer(GL_COPY_WRITE_BUFFER,out);glBufferData(GL_COPY_WRITE_BUFFER,static_cast<GLsizeiptr>((bytes+3)&~size_t(3)),nullptr,GL_DYNAMIC_COPY);
    glBindBuffer(GL_COPY_READ_BUFFER,initial);if(!p.destination_bytes)glCopyBufferSubData(GL_COPY_READ_BUFFER,GL_COPY_WRITE_BUFFER,0,0,static_cast<GLsizeiptr>(bytes));
    const auto args=arguments(p);argsBuffer=upload({reinterpret_cast<const uint8_t*>(args.data()),sizeof(args)});
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER,0,out);glBindBufferBase(GL_SHADER_STORAGE_BUFFER,1,a);glBindBufferBase(GL_SHADER_STORAGE_BUFFER,2,b);glBindBufferBase(GL_SHADER_STORAGE_BUFFER,3,argsBuffer);
    glUseProgram(d->program);glDispatchCompute((p.destination.width+15)/16,(p.destination.height+7)/8,1);glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT|GL_BUFFER_UPDATE_BARRIER_BIT);glFinish();
    const bool ok=glGetError()==GL_NO_ERROR;
    if(ownInitial)glDeleteBuffers(1,&initial);if(ownA)glDeleteBuffers(1,&a);if(ownB)glDeleteBuffers(1,&b);glDeleteBuffers(1,&argsBuffer);
    if(!ok){glDeleteBuffers(1,&out);return {};}return std::make_shared<Storage>(std::move(d),out,bytes);
}
}
std::shared_ptr<GpuPixelStorage> execute_resident(const ExactPixelPixels& p){return run(p);}
PixelKernelResult execute(const ExactPixelPixels& p){auto result=run(p);if(!result || result->size()!=p.destination.pixels.size())return {};std::vector<uint8_t> output(result->size());if(!result->read(output))return {};std::copy(output.begin(),output.end(),p.destination.pixels.begin());return {true,true};}
}
#endif
