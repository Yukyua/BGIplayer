// Android host accelerator. Engine integer arithmetic is shared with the CPU
// reference; EGL and packed SSBO words replace Metal's byte-addressed buffers.
#ifdef __ANDROID__
#include "bgiplayer/gpu_pixel_kernels.hpp"
#include "bgiplayer/debug.hpp"
#include "bgiplayer/sprite_scaled_alpha.hpp"
#include "bgiplayer/sprite_crossfade.hpp"
#include <EGL/egl.h>
#include <GLES3/gl31.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>
#include <limits>
#include <vector>
namespace bgiplayer {
namespace {
using Clock=std::chrono::steady_clock;
const char* compute_source=R"GLSL(#version 310 es
precision highp float;precision highp int;
layout(local_size_x=16,local_size_y=16) in;
layout(std430,binding=0) buffer Destination {uint d[];};
layout(std430,binding=1) readonly buffer Source {uint a[];};
layout(std430,binding=2) readonly buffer Second {uint b[];};
layout(std430,binding=3) readonly buffer Arguments {uint p[];};
uint byteA(uint i){return (a[i>>2u]>>((i&3u)*8u))&255u;}
uint byteB(uint i){return (b[i>>2u]>>((i&3u)*8u))&255u;}
ivec4 unpack(uint c){return ivec4(c&255u,(c>>8u)&255u,(c>>16u)&255u,c>>24u);}
uint pack(ivec4 c){return uint(c.x)&255u|(uint(c.y)&255u)<<8u|(uint(c.z)&255u)<<16u|(uint(c.w)&255u)<<24u;}
ivec4 sampleA(int x,int y){if(x<0||y<0||uint(x)>=p[20]||uint(y)>=p[21])return ivec4(0);return unpack(a[(uint(y)*p[4]+uint(x)*4u)>>2u]);}
void main(){uint x=gl_GlobalInvocationID.x,y=gl_GlobalInvocationID.y,kind=p[0];
 if(kind==1u||kind==3u)x*=2u;
 if(x>=p[1]||y>=p[2])return;
 uint di=(p[6]+y*p[3]+x*4u)>>2u,si=(p[7]+y*p[4]+x*4u)>>2u;
 if(kind==0u||kind==2u){
  uint qx=p[16]+x*p[18]+y*p[24],qy=p[17]+x*p[19]+y*p[25];
  if(kind==2u&&p[22]==0u){qx+=32768u;qy+=32768u;}
  int ix=int(qx)>>16,iy=int(qy)>>16;ivec4 c=sampleA(ix,iy);ivec4 old=unpack(d[di]);
  if(kind==0u){
   if(p[22]!=0u){int fx=int((qx&65535u)>>12u),fy=int((qy&65535u)>>12u);ivec4 bl=sampleA(ix,iy+1);ivec4 top=c+((sampleA(ix+1,iy)-c)*fx>>4);ivec4 bot=bl+((sampleA(ix+1,iy+1)-bl)*fx>>4);c=top+((bot-top)*fy>>4);}
   uint index=uint(c.w)>>1u;int weight=int(((index==127u?128u:index)*(256u-p[10]))>>8u);
   old.xyz+=((c.xyz-old.xyz)*weight)>>7;d[di]=pack(old);
  }else{
   vec4 f=vec4(c);if(p[22]!=0u){float fx=float((qx>>12u)&15u)/16.0,fy=float((qy>>12u)&15u)/16.0;f=mix(mix(f,vec4(sampleA(ix+1,iy)),fx),mix(vec4(sampleA(ix,iy+1)),vec4(sampleA(ix+1,iy+1)),fx),fy);}
   if(p[23]!=0u){float alpha=f.w*float(256u-p[10])/(255.0*256.0);old.xyz=ivec3(clamp(floor(mix(vec3(old.xyz),f.xyz,alpha)+0.5),0.0,255.0));d[di]=pack(old);}else d[di]=pack(ivec4(clamp(floor(f+0.5),0.0,255.0)));
  }return;
 }
 if(kind==1u){
  uint count=min(2u,p[1]-x);ivec4 color[2];color[0]=ivec4(0);color[1]=ivec4(0);
  for(uint i=0u;i<count;++i){uint bi=(p[8]+y*p[5]+(x+i)*4u)>>2u;ivec4 ca=unpack(a[si+i]),cb=unpack(b[bi]);ca.w=int((uint(ca.w)*(256u-p[10]))>>8u);cb.w=int((uint(cb.w)*(256u-p[10]))>>8u);ca.xyz=(ca.xyz*ca.w)>>8;cb.xyz=(cb.xyz*cb.w)>>8;color[i]=ca+(((cb-ca)*int(p[11]))>>8);}
  if(color[0].w==0&&(count==1u||color[1].w==0))return;
  for(uint i=0u;i<count;++i){ivec4 old=unpack(d[di+i]);old.xyz=clamp(color[i].xyz+((old.xyz*(256-color[i].w))>>8),0,255);old.w=0;d[di+i]=pack(old);}return;
 }
 if(kind==3u){
  uint count=min(2u,p[1]-x),aa=a[si]>>24u,ab=count==2u?a[si+1u]>>24u:0u;
  if(count==2u&&aa==0u&&ab==0u)return;
  if(count==2u&&(p[9]==1u?(aa>=254u&&ab>=254u):(aa==255u&&ab==255u))){d[di]=a[si];d[di+1u]=a[si+1u];return;}
  for(uint i=0u;i<count;++i){ivec4 c=unpack(a[si+i]),old=unpack(d[di+i]);uint alpha=uint(c.w);
   if(p[9]==1u){uint factor=alpha>>1u;if(factor==127u){old.xyz=c.xyz;if(count==1u)old.w=0;}else if(factor!=0u)old.xyz+=((c.xyz-old.xyz)*int(factor))>>7;}
   else{if(count==1u&&alpha==0u)continue;if(count==1u&&alpha==255u){d[di+i]=a[si+i];continue;}uint da=uint(old.w),dc=da*(256u-alpha),total=alpha*256u+dc;int sw=total==0u?0:int((alpha<<16u)/total),dw=total==0u?0:int((dc<<8u)/total);if(count==2u){uint fix=byteB(alpha*256u+da);if(fix==1u)--sw;else if(fix==2u)++sw;else if(fix==3u)--dw;else if(fix==4u)++dw;}old.xyz=((c.xyz*sw+old.xyz*dw)&ivec3(65535))>>8;old.w=int(total>>8u);}d[di+i]=pack(old);
  }return;
 }
 if(kind==4u){
  int factor=int(p[64u+byteB(p[8]+(p[30]+y)*p[5]+p[29]+x)]);if(factor==0)return;
  di=(p[6]+(p[27]+y)*p[3]+(p[26]+x)*4u)>>2u;si=(p[7]+(p[28]+y)*p[4]+(p[31]+x)*4u)>>2u;
  if(factor<0){d[di]=a[si];return;}ivec4 c=unpack(a[si]),old=unpack(d[di]);old.xyz=clamp(old.xyz+(((c.xyz-old.xyz)*factor)>>int(p[32])),0,255);d[di]=pack(old);return;
 }
 if(kind==5u){
  uint op=p[10],value=p[11];ivec4 old=unpack(d[di]),c=(op==3u||op==5u)?ivec4(0):unpack(a[si]);
  if(op==5u){d[di]=0u;}
  else if(op==0u){if(p[13]==2u&&p[14]==1u)c.w=255;else if(p[13]==1u&&p[14]==2u){uint alpha=uint(c.w)>>1u;if(alpha==127u)alpha=128u;c.xyz=(c.xyz*int(alpha))>>7;c.w=0;}d[di]=pack(c);}
  else if(op==1u)d[di]=pack((c*int(256u-value))>>8);
  else if(op==2u)d[di]=pack(c+(((old-c)*int(value>>1u))>>7));
  else if(op==4u){uint factor=((uint(c.w)>>1u)*(256u-value))>>8u;d[di]=pack(old+(((c-old)*int(factor))>>7));}
  else if(op==3u){c=unpack(p[12]);old.xyz=(old.xyz*int(256u-value)+c.xyz*int(value))>>8;if(p[12]==0u&&value!=0u)old.w=0;d[di]=pack(old);}
 }
}
)GLSL";
bool valid(const GlyphBitmapView& v,uint32_t bpp=4){return v.width&&v.height&&v.bytes_per_pixel==bpp&&v.stride>=uint64_t(v.width)*bpp&&uint64_t(v.byte_offset)+uint64_t(v.height-1)*v.stride+uint64_t(v.width)*bpp<=v.pixels.size()&&v.pixels.size()<=UINT32_MAX&&(bpp!=4||(v.byte_offset%4==0&&v.stride%4==0));}
bool overlap(std::span<uint8_t> d,std::span<const uint8_t> s){auto a=reinterpret_cast<uintptr_t>(d.data()),b=reinterpret_cast<uintptr_t>(s.data());return a<=b?b-a<d.size():a-b<s.size();}
struct RestoreContext {
 EGLDisplay display=eglGetCurrentDisplay();EGLContext context=eglGetCurrentContext();EGLSurface draw=eglGetCurrentSurface(EGL_DRAW),read=eglGetCurrentSurface(EGL_READ);
 ~RestoreContext(){if(display!=EGL_NO_DISPLAY)eglMakeCurrent(display,draw,read,context);else {auto current=eglGetCurrentDisplay();if(current!=EGL_NO_DISPLAY)eglMakeCurrent(current,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);}}
};
int32_t truncate_sse(double value){return value>=-2147483648.0&&value<2147483648.0?static_cast<int32_t>(value):INT32_MIN;}
}
struct GpuPixelKernels::Impl {
 EGLDisplay display=EGL_NO_DISPLAY;EGLContext context=EGL_NO_CONTEXT;EGLSurface surface=EGL_NO_SURFACE;GLuint program=0,buffers[4]{};size_t capacities[4]{};std::vector<uint8_t> readback;std::string name="CPU";
 ~Impl(){if(context!=EGL_NO_CONTEXT){RestoreContext restore;if(eglMakeCurrent(display,surface,surface,context)){glDeleteBuffers(4,buffers);if(program)glDeleteProgram(program);}eglMakeCurrent(display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);eglDestroyContext(display,context);}if(surface!=EGL_NO_SURFACE)eglDestroySurface(display,surface);/* SDL owns the shared EGLDisplay lifetime. */}
 PixelKernelResult dispatch(std::span<uint8_t> dst,std::span<const uint8_t> src,std::span<const uint8_t> second,std::vector<uint32_t>& args,bool paired=false,bool approximate=false){
  PixelKernelResult result;const bool timing=debug_enabled();auto start=timing?Clock::now():Clock::time_point{};
  if(!program||dst.empty()||args[1]==0||args[2]==0)return result;
  RestoreContext restore;if(!eglMakeCurrent(display,surface,surface,context))return result;
  GLint maximum=0;glGetIntegerv(GL_MAX_SHADER_STORAGE_BLOCK_SIZE,&maximum);
  std::span<const uint8_t> spans[]={dst,src,second,{reinterpret_cast<const uint8_t*>(args.data()),args.size()*4}};
  for(unsigned n=0;n<16 && glGetError()!=GL_NO_ERROR;++n){}
  auto upload=timing?Clock::now():Clock::time_point{};
  for(unsigned n=0;n<4;++n){size_t bytes=std::max<size_t>(4,(spans[n].size()+3)&~size_t(3));if(bytes>size_t(maximum))return result;glBindBuffer(GL_SHADER_STORAGE_BUFFER,buffers[n]);if(capacities[n]<bytes){glBufferData(GL_SHADER_STORAGE_BUFFER,static_cast<GLsizeiptr>(bytes),nullptr,GL_DYNAMIC_COPY);capacities[n]=bytes;}if(!spans[n].empty())glBufferSubData(GL_SHADER_STORAGE_BUFFER,0,static_cast<GLsizeiptr>(spans[n].size()),spans[n].data());glBindBufferBase(GL_SHADER_STORAGE_BUFFER,n,buffers[n]);}
  if(timing)result.upload_ns=std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-upload).count();
  if(glGetError()!=GL_NO_ERROR)return result;
  auto wait=timing?Clock::now():Clock::time_point{};
  glUseProgram(program);uint32_t width=paired?(args[1]+1)/2:args[1];glDispatchCompute((width+15)/16,(args[2]+15)/16,1);glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT|GL_SHADER_STORAGE_BARRIER_BIT);
  GLsync fence=glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE,0);if(!fence)return result;GLenum status=glClientWaitSync(fence,GL_SYNC_FLUSH_COMMANDS_BIT,1000000000ULL);glDeleteSync(fence);
  if(timing)result.wait_ns=std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-wait).count();
  if((status!=GL_ALREADY_SIGNALED&&status!=GL_CONDITION_SATISFIED)||glGetError()!=GL_NO_ERROR)return result;
  auto download=timing?Clock::now():Clock::time_point{};glBindBuffer(GL_SHADER_STORAGE_BUFFER,buffers[0]);void* mapped=glMapBufferRange(GL_SHADER_STORAGE_BUFFER,0,static_cast<GLsizeiptr>(dst.size()),GL_MAP_READ_BIT);if(!mapped)return result;
  // Publish only after the complete readback succeeds, preserving CPU fallback.
  readback.resize(dst.size());std::memcpy(readback.data(),mapped,readback.size());if(glUnmapBuffer(GL_SHADER_STORAGE_BUFFER)!=GL_TRUE||glGetError()!=GL_NO_ERROR)return result;std::memcpy(dst.data(),readback.data(),readback.size());
  if(timing){result.download_ns=std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-download).count();result.total_ns=std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-start).count();}result.success=true;result.used_gpu=true;result.approximate=approximate;return result;
 }
};
GpuPixelKernels::GpuPixelKernels(bool enabled):impl_(std::make_unique<Impl>()){
 if(!enabled)return;RestoreContext restore;
 impl_->display=eglGetDisplay(EGL_DEFAULT_DISPLAY);EGLint major=0,minor=0;if(impl_->display==EGL_NO_DISPLAY||!eglInitialize(impl_->display,&major,&minor))return;
 EGLint attrs[]={EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_ES3_BIT,EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_NONE};EGLConfig config;EGLint count=0;if(!eglChooseConfig(impl_->display,attrs,&config,1,&count)||!count)return;
 EGLint contextAttrs[]={EGL_CONTEXT_CLIENT_VERSION,3,EGL_NONE},surfaceAttrs[]={EGL_WIDTH,1,EGL_HEIGHT,1,EGL_NONE};impl_->surface=eglCreatePbufferSurface(impl_->display,config,surfaceAttrs);impl_->context=eglCreateContext(impl_->display,config,EGL_NO_CONTEXT,contextAttrs);
 if(impl_->surface==EGL_NO_SURFACE||impl_->context==EGL_NO_CONTEXT||!eglMakeCurrent(impl_->display,impl_->surface,impl_->surface,impl_->context))return;
 GLint glMajor=0,glMinor=0;glGetIntegerv(GL_MAJOR_VERSION,&glMajor);glGetIntegerv(GL_MINOR_VERSION,&glMinor);if(glMajor<3||(glMajor==3&&glMinor<1)){std::cerr<<"warning: ANDROID_GPU_UNAVAILABLE OpenGL ES 3.1 compute required; CPU composition selected\n";return;}
 GLuint shader=glCreateShader(GL_COMPUTE_SHADER);glShaderSource(shader,1,&compute_source,nullptr);glCompileShader(shader);GLint okay=0;glGetShaderiv(shader,GL_COMPILE_STATUS,&okay);if(!okay){char log[2048]{};glGetShaderInfoLog(shader,sizeof(log),nullptr,log);std::cerr<<"warning: ANDROID_GPU_SHADER "<<log<<'\n';glDeleteShader(shader);return;}
 GLuint program=glCreateProgram();glAttachShader(program,shader);glLinkProgram(program);glDeleteShader(shader);glGetProgramiv(program,GL_LINK_STATUS,&okay);if(!okay){glDeleteProgram(program);return;}impl_->program=program;glGenBuffers(4,impl_->buffers);if(auto name=glGetString(GL_RENDERER))impl_->name=reinterpret_cast<const char*>(name);
}
GpuPixelKernels::~GpuPixelKernels()=default;
bool GpuPixelKernels::available()const{return impl_->program!=0;}
std::string GpuPixelKernels::device_name()const{return impl_->name;}
PixelKernelResult GpuPixelKernels::scaled_alpha(const ScaledAlphaPixels& p){
 auto fallback=[&](){return PixelKernelResult{compose_scaled_alpha_bgra(p.destination,p.destination_width,p.destination_height,p.destination_stride,p.source,p.source_width,p.source_height,p.source_stride,p.x_q16,p.y_q16,p.origin_x_q16,p.origin_y_q16,p.scale_x_q16,p.scale_y_q16,p.parameter,p.filtered),false};};
 if(!available()||p.scale_x_q16<=32768||p.scale_y_q16<=32768||p.parameter>256||p.source_width>32767||p.source_height>32767||p.destination_width>32767||p.destination_height>32767||p.destination_stride<uint64_t(p.destination_width)*4||p.source_stride<uint64_t(p.source_width)*4||p.destination_stride%4||p.source_stride%4||p.source_stride>32764||p.destination.size()<uint64_t(p.destination_stride)*p.destination_height||p.source.size()<uint64_t(p.source_stride)*p.source_height||overlap(p.destination,p.source))return fallback();
 std::vector<uint32_t> a(64);a[0]=0;a[1]=p.destination_width;a[2]=p.destination_height;a[3]=p.destination_stride;a[4]=p.source_stride;a[10]=p.parameter;
 double ix=65536.0/double(p.scale_x_q16),iy=65536.0/double(p.scale_y_q16);volatile double tx=double(p.x_q16)/65536.0*ix,ty=double(p.y_q16)/65536.0*iy;
 a[16]=uint32_t(truncate_sse((double(p.origin_x_q16)/65536.0-tx)*65536.0))+(p.filtered?0u:32768u);a[17]=uint32_t(truncate_sse((double(p.origin_y_q16)/65536.0-ty)*65536.0))+(p.filtered?0u:32768u);a[18]=uint32_t(truncate_sse(ix*65536.0));a[25]=uint32_t(truncate_sse(iy*65536.0));a[20]=p.source_width;a[21]=p.source_height;a[22]=p.filtered;
 auto r=impl_->dispatch(p.destination,p.source,{},a);return r.success?r:fallback();
}
PixelKernelResult GpuPixelKernels::crossfade(const CrossfadePixels& p){
 auto fallback=[&](){return PixelKernelResult{blend_original_sprite_crossfade(p.destination,p.first,p.second,p.mix,p.opacity),false};};
 uint32_t w=std::min(p.first.width,p.second.width),h=std::min(p.first.height,p.second.height);
 if(!available()||!valid(p.destination)||!valid(p.first)||!valid(p.second)||p.destination.format!=1||p.first.format!=2||p.second.format!=2||p.destination.width<w||p.destination.height<h||p.mix<0||p.mix>256||p.opacity>=256||overlap(p.destination.pixels,p.first.pixels)||overlap(p.destination.pixels,p.second.pixels))return fallback();
 std::vector<uint32_t>a(64);a[0]=1;a[1]=w;a[2]=h;a[3]=p.destination.stride;a[4]=p.first.stride;a[5]=p.second.stride;a[6]=p.destination.byte_offset;a[7]=p.first.byte_offset;a[8]=p.second.byte_offset;a[10]=p.opacity;a[11]=uint32_t(p.mix);auto r=impl_->dispatch(p.destination.pixels,p.first.pixels,p.second.pixels,a,true);return r.success?r:fallback();
}
PixelKernelResult GpuPixelKernels::affine(const AffinePixels& p){
 if(!available()||p.parameter>256||p.destination_width>32767||p.destination_height>32767||p.source_width>32767||p.source_height>32767||p.destination_stride<uint64_t(p.destination_width)*4||p.source_stride<uint64_t(p.source_width)*4||p.destination_stride%4||p.source_stride%4||p.destination.size()<uint64_t(p.destination_stride)*p.destination_height||p.source.size()<uint64_t(p.source_stride)*p.source_height||overlap(p.destination,p.source))return {};
 std::vector<uint32_t>a(64);a[0]=2;a[1]=p.destination_width;a[2]=p.destination_height;a[3]=p.destination_stride;a[4]=p.source_stride;a[10]=p.parameter;for(unsigned i=0;i<6;++i)a[16+i]=uint32_t(p.matrix[i]);a[24]=a[20];a[25]=a[21];a[20]=p.source_width;a[21]=p.source_height;a[22]=p.filtered;a[23]=p.blend_alpha;return impl_->dispatch(p.destination,p.source,{},a,false,true);
}
PixelKernelResult GpuPixelKernels::layer_composite(const LayerCompositePixels& p){
 if(!available()||!valid(p.destination)||!valid(p.source)||p.source.format!=2||(p.destination.format!=1&&p.destination.format!=2)||p.destination.width!=p.source.width||p.destination.height!=p.source.height||p.reciprocal_corrections.size()!=65536||overlap(p.destination.pixels,p.source.pixels))return {};
 std::vector<uint32_t>a(64);a[0]=3;a[1]=p.destination.width;a[2]=p.destination.height;a[3]=p.destination.stride;a[4]=p.source.stride;a[6]=p.destination.byte_offset;a[7]=p.source.byte_offset;a[9]=p.destination.format;return impl_->dispatch(p.destination.pixels,p.source.pixels,p.reciprocal_corrections,a,true);
}
PixelKernelResult GpuPixelKernels::background_mask(const BackgroundMaskPixels& p){
 if(!available()||!valid(p.destination)||!valid(p.source)||!valid(p.mask,1)||p.destination.format!=1||p.source.format!=1||p.mask.format!=3||p.left>=p.right||p.top>=p.bottom||p.right>p.destination.width||p.bottom>p.destination.height||uint64_t(p.source_left)+p.right-p.left>p.source.width||uint64_t(p.source_top)+p.bottom-p.top>p.source.height||uint64_t(p.mask_left)+p.right-p.left>p.mask.width||uint64_t(p.mask_top)+p.bottom-p.top>p.mask.height||(p.shift!=7&&p.shift!=12)||overlap(p.destination.pixels,p.source.pixels)||overlap(p.destination.pixels,p.mask.pixels))return {};
 for(auto f:p.factors)if(f< -1||f>32767)return {};
 std::vector<uint32_t>a(320);a[0]=4;a[1]=p.right-p.left;a[2]=p.bottom-p.top;a[3]=p.destination.stride;a[4]=p.source.stride;a[5]=p.mask.stride;a[6]=p.destination.byte_offset;a[7]=p.source.byte_offset;a[8]=p.mask.byte_offset;a[26]=p.left;a[27]=p.top;a[28]=p.source_top;a[29]=p.mask_left;a[30]=p.mask_top;a[31]=p.source_left;a[32]=p.shift;for(unsigned i=0;i<256;++i)a[64+i]=uint32_t(p.factors[i]);return impl_->dispatch(p.destination.pixels,p.source.pixels,p.mask.pixels,a);
}
PixelKernelResult GpuPixelKernels::pixel_transfer(const PixelTransferPixels& p){
 if(!available()||!valid_pixel_transfer(p)||p.destination.byte_offset%4||p.destination.stride%4)return {};
 const auto op=static_cast<uint32_t>(p.operation);if(op!=3&&op!=5&&(p.source.byte_offset%4||p.source.stride%4))return {};
 std::vector<uint32_t>a(64);a[0]=5;a[1]=p.destination.width;a[2]=p.destination.height;a[3]=p.destination.stride;a[4]=p.source.stride;a[6]=p.destination.byte_offset;a[7]=p.source.byte_offset;a[10]=op;a[11]=p.parameter;a[12]=p.color;a[13]=p.destination.format;a[14]=p.source.format;return impl_->dispatch(p.destination.pixels,p.source.pixels,{},a);
}
}
#endif
