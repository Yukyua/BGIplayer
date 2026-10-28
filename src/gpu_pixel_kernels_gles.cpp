// Android host accelerator. Engine integer arithmetic is shared with the CPU
// reference; EGL and packed SSBO words replace Metal's byte-addressed buffers.
#ifdef __ANDROID__
#include "bgiplayer/gpu_pixel_kernels.hpp"
#include "bgiplayer/debug.hpp"
#include "bgiplayer/rcpss_estimate_table.hpp"
#include "bgiplayer/sprite_scaled_alpha.hpp"
#include "bgiplayer/sprite_crossfade.hpp"
#include <EGL/egl.h>
#include <GLES3/gl31.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>
#include <limits>
#include <mutex>
#include <vector>
namespace bgiplayer {
namespace {
using Clock=std::chrono::steady_clock;
const char* compute_source=R"GLSL(#version 310 es
precision highp float;precision highp int;
layout(local_size_x=16,local_size_y=8) in;
layout(std430,binding=0) buffer Destination {uint d[];};
layout(std430,binding=1) readonly buffer Source {uint a[];};
layout(std430,binding=2) readonly buffer Second {uint b[];};
layout(std430,binding=3) readonly buffer Arguments {uint p[];};
uint byteA(uint i){return (a[i>>2u]>>((i&3u)*8u))&255u;}
uint byteB(uint i){return (b[i>>2u]>>((i&3u)*8u))&255u;}
ivec4 unpack(uint c){return ivec4(c&255u,(c>>8u)&255u,(c>>16u)&255u,c>>24u);}
uint pack(ivec4 c){return uint(c.x)&255u|(uint(c.y)&255u)<<8u|(uint(c.z)&255u)<<16u|(uint(c.w)&255u)<<24u;}
// Materialize SSBO scalars before passing them to value parameters. Some
// mobile compilers incorrectly carry the block's readonly qualifier into
// unpack(a[index]) and reject the call with S0001.
ivec4 sampleA(int x,int y){if(x<0||y<0||uint(x)>=p[20]||uint(y)>=p[21])return ivec4(0);uint color=a[(uint(y)*p[4]+uint(x)*4u)>>2u];return unpack(color);}
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
  for(uint i=0u;i<count;++i){uint bi=(p[8]+y*p[5]+(x+i)*4u)>>2u;uint first=a[si+i],second=b[bi];ivec4 ca=unpack(first),cb=unpack(second);ca.w=int((uint(ca.w)*(256u-p[10]))>>8u);cb.w=int((uint(cb.w)*(256u-p[10]))>>8u);ca.xyz=(ca.xyz*ca.w)>>8;cb.xyz=(cb.xyz*cb.w)>>8;color[i]=ca+(((cb-ca)*int(p[11]))>>8);}
  if(color[0].w==0&&(count==1u||color[1].w==0))return;
  for(uint i=0u;i<count;++i){ivec4 old=unpack(d[di+i]);old.xyz=clamp(color[i].xyz+((old.xyz*(256-color[i].w))>>8),0,255);old.w=0;d[di+i]=pack(old);}return;
 }
 if(kind==3u){
  uint count=min(2u,p[1]-x),aa=a[si]>>24u,ab=count==2u?a[si+1u]>>24u:0u;
  if(count==2u&&aa==0u&&ab==0u)return;
  if(count==2u&&(p[9]==1u?(aa>=254u&&ab>=254u):(aa==255u&&ab==255u))){d[di]=a[si];d[di+1u]=a[si+1u];return;}
  for(uint i=0u;i<count;++i){uint color=a[si+i];ivec4 c=unpack(color),old=unpack(d[di+i]);uint alpha=uint(c.w);
   if(p[9]==1u){uint factor=alpha>>1u;if(factor==127u){old.xyz=c.xyz;if(count==1u)old.w=0;}else if(factor!=0u)old.xyz+=((c.xyz-old.xyz)*int(factor))>>7;}
   else{if(count==1u&&alpha==0u)continue;if(count==1u&&alpha==255u){d[di+i]=a[si+i];continue;}uint da=uint(old.w),dc=da*(256u-alpha),total=alpha*256u+dc;int sw=total==0u?0:int((alpha<<16u)/total),dw=total==0u?0:int((dc<<8u)/total);if(count==2u){uint fix=byteB(alpha*256u+da);if(fix==1u)--sw;else if(fix==2u)++sw;else if(fix==3u)--dw;else if(fix==4u)++dw;}old.xyz=((c.xyz*sw+old.xyz*dw)&ivec3(65535))>>8;old.w=int(total>>8u);}d[di+i]=pack(old);
  }return;
 }
 if(kind==4u){
  int factor=int(p[64u+byteB(p[8]+(p[30]+y)*p[5]+p[29]+x)]);if(factor==0)return;
  di=(p[6]+(p[27]+y)*p[3]+(p[26]+x)*4u)>>2u;si=(p[7]+(p[28]+y)*p[4]+(p[31]+x)*4u)>>2u;
  if(factor<0){d[di]=a[si];return;}uint color=a[si];ivec4 c=unpack(color),old=unpack(d[di]);old.xyz=clamp(old.xyz+(((c.xyz-old.xyz)*factor)>>int(p[32])),0,255);d[di]=pack(old);return;
 }
 if(kind==5u){
  uint op=p[10],value=p[11],color=(op==3u||op==5u)?0u:a[si];ivec4 old=unpack(d[di]),c=unpack(color);
  if(op==5u){d[di]=0u;}
  else if(op==0u){if(p[13]==2u&&p[14]==1u)c.w=255;else if(p[13]==1u&&p[14]==2u){uint alpha=uint(c.w)>>1u;if(alpha==127u)alpha=128u;c.xyz=(c.xyz*int(alpha))>>7;c.w=0;}d[di]=pack(c);}
  else if(op==1u)d[di]=pack((c*int(256u-value))>>8);
  else if(op==2u)d[di]=pack(c+(((old-c)*int(value>>1u))>>7));
  else if(op==4u){uint factor=((uint(c.w)>>1u)*(256u-value))>>8u;d[di]=pack(old+(((c-old)*int(factor))>>7));}
  else if(op==3u){uint tint=p[12];c=unpack(tint);old.xyz=(old.xyz*int(256u-value)+c.xyz*int(value))>>8;if(p[12]==0u&&value!=0u)old.w=0;d[di]=pack(old);}
 }
}
)GLSL";
// Two separable passes retain the original byte quantization between axes.
// Each invocation slides across an entire row/column, independent of radius.
const char* effector_blur_source=R"GLSL(#version 310 es
precision highp int;
layout(local_size_x=64) in;
layout(std430,binding=0) writeonly buffer Destination {uint d[];};
layout(std430,binding=1) readonly buffer Source {uint a[];};
layout(std430,binding=2) readonly buffer Horizontal {uint h[];};
layout(std430,binding=3) readonly buffer Arguments {uint p[];};
layout(location=0) uniform uint vertical;
layout(location=1) uniform uint pixelWise;
uvec4 halfPixel(uint c){return uvec4(c&255u,(c>>8u)&255u,(c>>16u)&255u,c>>24u)>>1u;}
uint quantize(uvec4 sums){uvec4 c=((sums*p[11])>>16u)&uvec4(255u);return c.x|(c.y<<8u)|(c.z<<16u)|(c.w<<24u);}
uint sampleLine(int at,uint line,bool v){
 int count=int(v?p[21]:p[20]);
 if(at<0||at>=count){if((p[12]&1u)==0u)return 0u;at=clamp(at,0,count-1);}
 uint index=uint(at);
 if(v&&p[12]>=4u)return h[index*p[20]+line];
 return a[(p[7]+(v?index*p[4]+line*4u:line*p[4]+index*4u))>>2u];
}
void main(){
 if(pixelWise!=0u){
  uint x=gl_GlobalInvocationID.x,y=gl_GlobalInvocationID.y;
  if(x>=p[20]||y>=p[21])return;
  bool v=vertical!=0u;uvec4 sums=uvec4(0u);
  for(int delta=-int(p[10]);delta<=int(p[10]);++delta)
   sums+=halfPixel(sampleLine(int(v?y:x)+delta,v?x:y,v));
  uint index=!v&&p[12]>=4u?y*p[20]+x:(p[6]+y*p[3]+x*4u)>>2u;
  d[index]=quantize(sums);return;
 }
 uint line=gl_GlobalInvocationID.x,width=p[20],height=p[21];
 bool v=vertical!=0u;uint count=v?height:width,lines=v?width:height;
 if(line>=lines)return;
 int radius=int(p[10]);uvec4 sums=uvec4(0u);
 for(int i=-radius;i<=radius;++i)sums+=halfPixel(sampleLine(i,line,v));
 for(uint at=0u;at<count;++at){
  uint index=!v&&p[12]>=4u?line*width+at:(p[6]+(v?at*p[3]+line*4u:line*p[3]+at*4u))>>2u;
  d[index]=quantize(sums);
  sums-=halfPixel(sampleLine(int(at)-radius,line,v));
  sums+=halfPixel(sampleLine(int(at)+radius+1,line,v));
 }
}
)GLSL";
const char* effector_displacement_source=R"GLSL(#version 310 es
precision highp int;
layout(local_size_x=16,local_size_y=8) in;
layout(std430,binding=0) writeonly buffer Destination {uint d[];};
layout(std430,binding=1) readonly buffer Source {uint a[];};
layout(std430,binding=2) readonly buffer FirstMap {uint m[];};
layout(std430,binding=3) readonly buffer SecondMap {uint n[];};
layout(location=0) uniform uint p[64];
int s16(uint value){return int(value<<16u)>>16;}
ivec4 unpack(uint c){return ivec4(c&255u,(c>>8u)&255u,(c>>16u)&255u,c>>24u);}
uint pack(ivec4 c){return uint(c.x)|(uint(c.y)<<8u)|(uint(c.z)<<16u)|(uint(c.w)<<24u);}
ivec4 sampleSource(int x,int y){if(x<0||y<0||uint(x)>=p[20]||uint(y)>=p[21])return ivec4(0);uint color=a[(p[7]+uint(y)*p[4]+uint(x)*4u)>>2u];return unpack(color);}
void main(){uint x=gl_GlobalInvocationID.x,y=gl_GlobalInvocationID.y;if(x>=p[1]||y>=p[2])return;
 uint di=(p[6]+y*p[3]+x*4u)>>2u,strength=p[10];bool second=p[12]!=0u,bilinear=p[11]!=0u;
 if(!second&&strength==0u){d[di]=a[(p[7]+y*p[4]+x*4u)>>2u];return;}
 uint ma=m[(p[8]+y*p[5]+x*4u)>>2u],mb=second?n[(p[14]+y*p[13]+x*4u)>>2u]:0u;
 bool dual=second&&strength>0u&&strength<256u;uint selected=second&&strength==256u?mb:ma;
 uint effective=second&&(strength==0u||strength==256u)?256u:strength;
 int bits=bilinear?(dual||effective<256u?3:4):0;ivec2 offset=ivec2(0);
 for(uint component=0u;component<2u;++component){int value=s16(selected>>(component*16u)),q;
  if(dual){int difference=s16(uint(s16(mb>>(component*16u))-value));q=s16(uint((value>>1)+((difference*int(strength<<7u))>>16)));if(!bilinear)q=s16(uint(q+4))>>3;}
  else if(bilinear)q=effective==256u?value:(value*int(effective<<7u))>>16;
  else q=(value*int(effective)+8)>>12;
  offset[int(component)]=q;
 }
 ivec2 fraction=bilinear?offset&ivec2((1<<bits)-1):ivec2(0);
 int sx=s16(uint(int(x)+(offset.x>>bits))),sy=s16(uint(int(y)+(offset.y>>bits)));
 ivec4 color=sampleSource(sx,sy);
 if(bilinear){ivec4 top=color+((sampleSource(sx+1,sy)-color)*fraction.x>>bits),bottom0=sampleSource(sx,sy+1);
  ivec4 bottom=bottom0+((sampleSource(sx+1,sy+1)-bottom0)*fraction.x>>bits);color=top+((bottom-top)*fraction.y>>bits);}
 d[di]=pack(clamp(color,0,255));
}
)GLSL";
const char* effector_vector_source=R"GLSL(#version 310 es
precision highp int;
layout(local_size_x=16,local_size_y=8) in;
layout(std430,binding=0) writeonly buffer Destination {uint d[];};
layout(std430,binding=1) readonly buffer Source {uint a[];};
layout(std430,binding=2) readonly buffer NormalMap {uint m[];};
layout(std430,binding=3) readonly buffer Coefficients {uint c[];};
layout(location=0) uniform uint p[64];
int s16(uint value){return int(value<<16u)>>16;}
uint mapByte(uint i){return (m[i>>2u]>>((i&3u)*8u))&255u;}
uint mapWord(uint i){return mapByte(i)|(mapByte(i+1u)<<8u);}
ivec4 unpack(uint color){return ivec4(color&255u,(color>>8u)&255u,(color>>16u)&255u,color>>24u);}
uint pack(ivec4 color){return uint(color.x)|(uint(color.y)<<8u)|(uint(color.z)<<16u)|(uint(color.w)<<24u);}
ivec4 sampleSource(int x,int y){
 uint index;
 if(p[11]!=0u){if(x<0||y<0||uint(x)>=p[1]||uint(y)>=p[2])return ivec4(0);index=uint(y)*(p[4]>>2u)+uint(x);}
 else{
  int high,low;imulExtended(y,int(p[4]>>2u),high,low);
  index=uint(low)+uint(x);high+=(x<0?-1:0)+(index<uint(low)?1:0);
  if(high!=0||index>=p[14])return ivec4(0);
 }
 uint color=a[(p[7]>>2u)+index];return unpack(color);
}
void main(){uint x=gl_GlobalInvocationID.x,y=gl_GlobalInvocationID.y;if(x>=p[1]||y>=p[2])return;
 uint mi=p[8]+y*p[5]+x*6u,index=mapWord(mi+4u);if(index>=p[12])return;
 uint coefficient=c[index];int qx=s16(mapWord(mi))*s16(coefficient),qy=s16(mapWord(mi+2u))*s16(coefficient>>16u);
 bool bilinear=p[10]!=0u;int bias=bilinear?0:32768;
 int sx=s16(uint(int(x)+((qx+bias)>>16))),sy=s16(uint(int(y)+((qy+bias)>>16)));
 ivec4 color=sampleSource(sx,sy);
 if(bilinear){int fx=int((uint(qx)>>12u)&15u),fy=int((uint(qy)>>12u)&15u);
  ivec4 top=color+((sampleSource(sx+1,sy)-color)*fx>>4),bl=sampleSource(sx,sy+1);
  ivec4 bottom=bl+((sampleSource(sx+1,sy+1)-bl)*fx>>4);color=top+((bottom-top)*fy>>4);}
 d[(p[6]+y*p[3]+x*4u)>>2u]=pack(clamp(color,0,255));
}
)GLSL";
const char* effector_transform_source=R"GLSL(#version 310 es
precision highp int;
layout(local_size_x=16,local_size_y=8) in;
layout(std430,binding=0) writeonly buffer Destination {uint d[];};
layout(std430,binding=1) readonly buffer Source {uint a[];};
layout(location=0) uniform uint p[64];
ivec4 unpack(uint c){return ivec4(c&255u,(c>>8u)&255u,(c>>16u)&255u,c>>24u);}
uint pack(ivec4 c){return uint(c.x)|(uint(c.y)<<8u)|(uint(c.z)<<16u)|(uint(c.w)<<24u);}
ivec4 sampleSource(int x,int y){if(x<0||y<0||uint(x)>=p[12]||uint(y)>=p[13])return ivec4(0);uint color=a[(p[7]+uint(y)*p[4]+uint(x)*4u)>>2u];return unpack(color);}
void main(){uint x=gl_GlobalInvocationID.x,y=gl_GlobalInvocationID.y;if(x>=p[1]||y>=p[2])return;
 uint qx=p[16]+x*p[18]+y*p[20],qy=p[17]+x*p[19]+y*p[21];
 bool bilinear=p[10]!=0u;if(!bilinear){qx+=32768u;qy+=32768u;}
 int ix=int(qx)>>16,iy=int(qy)>>16;ivec4 color=sampleSource(ix,iy);
 if(bilinear){int fx=int((qx>>12u)&15u),fy=int((qy>>12u)&15u);
  ivec4 top=color+((sampleSource(ix+1,iy)-color)*fx>>4),bl=sampleSource(ix,iy+1);
  ivec4 bottom=bl+((sampleSource(ix+1,iy+1)-bl)*fx>>4);color=top+((bottom-top)*fy>>4);}
 d[(p[6]+y*p[3]+x*4u)>>2u]=pack(clamp(color,0,255));
}
)GLSL";
const char* effector_temporal_source=R"GLSL(#version 310 es
precision highp int;precision highp float;
layout(local_size_x=16,local_size_y=8) in;
layout(std430,binding=0) buffer Scene {uint d[];};
layout(std430,binding=1) buffer History {uint a[];};
layout(std430,binding=3) readonly buffer Reciprocal {uint r[];};
layout(location=0) uniform uint p[64];
ivec4 unpack(uint c){return ivec4(c&255u,(c>>8u)&255u,(c>>16u)&255u,c>>24u);}
uint pack(ivec4 c){return uint(c.x)|(uint(c.y)<<8u)|(uint(c.z)<<16u)|(uint(c.w)<<24u);}
float reciprocal(uint total){uint bits=floatBitsToUint(total==0u?1.0:float(total)/65536.0);return uintBitsToFloat(r[(bits>>11u)&4095u]-((((bits>>23u)&255u)-127u)<<23u));}
void main(){uint x=gl_GlobalInvocationID.x*2u,y=gl_GlobalInvocationID.y;if(x>=p[1]||y>=p[2])return;
 uint count=min(2u,p[1]-x),strength=p[10],si=(p[6]+y*p[3]+x*4u)>>2u,hi=(p[7]+y*p[4]+x*4u)>>2u;
 uint colors[2];colors[0]=d[si];colors[1]=count==2u?d[si+1u]:0u;
 if(strength>0u&&strength<=256u&&x<p[12]&&y<p[13]){
  uint blendCount=min(2u,p[12]-x),h0=a[hi],h1=blendCount==2u?a[hi+1u]:0u;
  bool skip=p[11]==2u&&(h0>>24u)==0u&&(blendCount==1u||(h1>>24u)==0u);
  if(!skip)for(uint i=0u;i<blendCount;++i){uint old=i==0u?h0:h1;ivec4 history=unpack(old),scene=unpack(colors[i]);
   if(strength==256u&&(p[11]==1u||(blendCount==1u&&history.w==255)||(blendCount==2u&&(h0>>24u)==255u&&(h1>>24u)==255u)))colors[i]=old;
   else if(p[11]==1u)colors[i]=pack(history+((scene-history)*int((256u-strength)>>1u)>>7));
   else{uint ha=uint(history.w),sa=uint(scene.w),hw,sw,alpha;
    if(blendCount==2u){uint first=ha*strength,second=sa*(65536u-first),total=(first<<8u)+second;float estimate=reciprocal(total);
     hw=uint(estimate*float(first));sw=uint((estimate*float(second))/256.0);alpha=total>>16u;}
    else{uint first=ha*strength,second=(sa*(65536u-first))>>8u,total=first+second;
     if(total==0u){hw=0u;sw=0u;alpha=0u;}else{hw=(first<<8u)/total;sw=(second<<8u)/total;alpha=total>>8u;}}
    ivec4 result=(history*int(hw)+scene*int(sw))&ivec4(65535);result=result>>8;result.w=int(alpha);colors[i]=pack(result);
   }
  }
 }
 for(uint i=0u;i<count;++i){d[si+i]=colors[i];a[hi+i]=colors[i];}
}
)GLSL";
GLuint make_compute_program(const char* source,const char* label){
 GLuint shader=glCreateShader(GL_COMPUTE_SHADER);glShaderSource(shader,1,&source,nullptr);glCompileShader(shader);
 GLint okay=0;glGetShaderiv(shader,GL_COMPILE_STATUS,&okay);
 if(!okay){char log[2048]{};glGetShaderInfoLog(shader,sizeof(log),nullptr,log);std::cerr<<"warning: ANDROID_GPU_SHADER kernel="<<label<<" "<<log<<'\n';glDeleteShader(shader);return 0;}
 GLuint program=glCreateProgram();glAttachShader(program,shader);glLinkProgram(program);glDeleteShader(shader);glGetProgramiv(program,GL_LINK_STATUS,&okay);
 if(!okay){char log[2048]{};glGetProgramInfoLog(program,sizeof(log),nullptr,log);std::cerr<<"warning: ANDROID_GPU_LINK kernel="<<label<<" "<<log<<'\n';glDeleteProgram(program);return 0;}
 return program;
}
bool valid(const GlyphBitmapView& v,uint32_t bpp=4,size_t storage_bytes=0){const size_t bytes=storage_bytes?storage_bytes:v.pixels.size();return v.width&&v.height&&v.bytes_per_pixel==bpp&&v.stride>=uint64_t(v.width)*bpp&&uint64_t(v.byte_offset)+uint64_t(v.height-1)*v.stride+uint64_t(v.width)*bpp<=bytes&&bytes<=UINT32_MAX&&(bpp!=4||(v.byte_offset%4==0&&v.stride%4==0));}
bool overlap(std::span<uint8_t> d,std::span<const uint8_t> s){auto a=reinterpret_cast<uintptr_t>(d.data()),b=reinterpret_cast<uintptr_t>(s.data());return a<=b?b-a<d.size():a-b<s.size();}
struct RestoreContext {
 EGLDisplay display=eglGetCurrentDisplay();EGLContext context=eglGetCurrentContext();EGLSurface draw=eglGetCurrentSurface(EGL_DRAW),read=eglGetCurrentSurface(EGL_READ);
 ~RestoreContext(){if(display!=EGL_NO_DISPLAY)eglMakeCurrent(display,draw,read,context);else {auto current=eglGetCurrentDisplay();if(current!=EGL_NO_DISPLAY)eglMakeCurrent(current,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);}}
};
int32_t truncate_sse(double value){return value>=-2147483648.0&&value<2147483648.0?static_cast<int32_t>(value):INT32_MIN;}
}
struct GlesDevice {
 EGLDisplay display=EGL_NO_DISPLAY;EGLContext context=EGL_NO_CONTEXT;EGLSurface surface=EGL_NO_SURFACE;GLuint program=0,blur_program=0,displacement_program=0,vector_program=0,transform_program=0,temporal_program=0,buffers[5]{};size_t capacities[5]{};std::recursive_mutex mutex;std::string name="CPU";
 ~GlesDevice(){if(context!=EGL_NO_CONTEXT){RestoreContext restore;if(eglMakeCurrent(display,surface,surface,context)){glDeleteBuffers(5,buffers);if(program)glDeleteProgram(program);if(blur_program)glDeleteProgram(blur_program);if(displacement_program)glDeleteProgram(displacement_program);if(vector_program)glDeleteProgram(vector_program);if(transform_program)glDeleteProgram(transform_program);if(temporal_program)glDeleteProgram(temporal_program);}eglMakeCurrent(display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);eglDestroyContext(display,context);}if(surface!=EGL_NO_SURFACE)eglDestroySurface(display,surface);/* SDL owns the shared EGLDisplay lifetime. */}
 PixelKernelResult dispatch(std::span<uint8_t> dst,std::span<const uint8_t> src,std::span<const uint8_t> second,std::vector<uint32_t>& args,bool paired=false,bool approximate=false,GLuint resident_source=0,bool separable_blur=false,GLuint special_program=0,std::span<const uint8_t> auxiliary={},std::span<uint8_t> second_output={}){
  std::lock_guard lock(mutex);PixelKernelResult result;const bool timing=debug_enabled();auto start=timing?Clock::now():Clock::time_point{};
  if(!program||(separable_blur&&!blur_program)||dst.empty()||args[1]==0||args[2]==0)return result;
  RestoreContext restore;if(!eglMakeCurrent(display,surface,surface,context))return result;
  GLint maximum=0;glGetIntegerv(GL_MAX_SHADER_STORAGE_BLOCK_SIZE,&maximum);
  std::span<const uint8_t> spans[]={dst,src,second,{reinterpret_cast<const uint8_t*>(args.data()),args.size()*4},auxiliary};
  for(unsigned n=0;n<16 && glGetError()!=GL_NO_ERROR;++n){}
  auto upload=timing?Clock::now():Clock::time_point{};
  for(unsigned n=0;n<(special_program?5u:4u);++n){if(special_program&&n==3)continue;if(n==1&&resident_source){glBindBufferBase(GL_SHADER_STORAGE_BUFFER,1,resident_source);continue;}size_t bytes=separable_blur&&n==2&&args[12]>=4?uint64_t(args[20])*args[21]*4:std::max<size_t>(4,(spans[n].size()+3)&~size_t(3));if(bytes>size_t(maximum))return result;glBindBuffer(GL_SHADER_STORAGE_BUFFER,buffers[n]);if(capacities[n]<bytes){glBufferData(GL_SHADER_STORAGE_BUFFER,static_cast<GLsizeiptr>(bytes),nullptr,GL_DYNAMIC_COPY);capacities[n]=bytes;}if(!spans[n].empty())glBufferSubData(GL_SHADER_STORAGE_BUFFER,0,static_cast<GLsizeiptr>(spans[n].size()),spans[n].data());glBindBufferBase(GL_SHADER_STORAGE_BUFFER,n==4?3:n,buffers[n]);}
  if(timing)result.upload_ns=std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-upload).count();
  if(glGetError()!=GL_NO_ERROR)return result;
  auto wait=timing?Clock::now():Clock::time_point{};
  if(separable_blur){
   glUseProgram(blur_program);
   const bool per_pixel=args[10]<=8;
   glUniform1ui(1,per_pixel?1u:0u);
   const auto blur_pass=[&](bool vertical){
    glUniform1ui(0,vertical?1u:0u);
    if(per_pixel)glDispatchCompute((args[20]+63)/64,args[21],1);
    else glDispatchCompute(((vertical?args[20]:args[21])+63)/64,1,1);
   };
   if(args[12]>=4){
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER,0,buffers[2]);
    blur_pass(false);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER,0,buffers[0]);
    blur_pass(true);
   }else{
    blur_pass(args[12]>=2);
   }
  }else if(special_program){glUseProgram(special_program);glUniform1uiv(0,static_cast<GLsizei>(args.size()),args.data());uint32_t width=paired?(args[1]+1)/2:args[1];glDispatchCompute((width+15)/16,(args[2]+7)/8,1);}
  else{glUseProgram(program);uint32_t width=paired?(args[1]+1)/2:args[1];glDispatchCompute((width+15)/16,(args[2]+7)/8,1);}
  glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT|GL_SHADER_STORAGE_BARRIER_BIT);
  GLsync fence=glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE,0);if(!fence)return result;GLenum status=glClientWaitSync(fence,GL_SYNC_FLUSH_COMMANDS_BIT,1000000000ULL);glDeleteSync(fence);
  if(timing)result.wait_ns=std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-wait).count();
  if((status!=GL_ALREADY_SIGNALED&&status!=GL_CONDITION_SATISFIED)||glGetError()!=GL_NO_ERROR)return result;
  auto download=timing?Clock::now():Clock::time_point{};glBindBuffer(GL_SHADER_STORAGE_BUFFER,buffers[0]);void* mapped=glMapBufferRange(GL_SHADER_STORAGE_BUFFER,0,static_cast<GLsizeiptr>(dst.size()),GL_MAP_READ_BIT);if(!mapped)return result;
  // Publish only after the complete readback succeeds, preserving CPU fallback.
  std::vector<uint8_t> readback(dst.size());std::memcpy(readback.data(),mapped,readback.size());if(glUnmapBuffer(GL_SHADER_STORAGE_BUFFER)!=GL_TRUE||glGetError()!=GL_NO_ERROR)return result;
  std::vector<uint8_t> secondary_readback;
  if(!second_output.empty()){
   glBindBuffer(GL_SHADER_STORAGE_BUFFER,buffers[1]);mapped=glMapBufferRange(GL_SHADER_STORAGE_BUFFER,0,static_cast<GLsizeiptr>(second_output.size()),GL_MAP_READ_BIT);if(!mapped)return result;
   secondary_readback.resize(second_output.size());std::memcpy(secondary_readback.data(),mapped,secondary_readback.size());
   if(glUnmapBuffer(GL_SHADER_STORAGE_BUFFER)!=GL_TRUE||glGetError()!=GL_NO_ERROR)return result;
  }
  std::memcpy(dst.data(),readback.data(),readback.size());if(!second_output.empty())std::memcpy(second_output.data(),secondary_readback.data(),secondary_readback.size());
  if(timing){result.download_ns=std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-download).count();result.total_ns=std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-start).count();}result.success=true;result.used_gpu=true;result.approximate=approximate;return result;
 }
};
namespace {
class GlesPixelStorage final : public GpuPixelStorage {
public:
 GlesPixelStorage(std::shared_ptr<GlesDevice> device,GLuint buffer,size_t bytes)
     :device_(std::move(device)),buffer_(buffer),bytes_(bytes){}
 ~GlesPixelStorage() override {
  std::lock_guard lock(device_->mutex);RestoreContext restore;
  if(eglMakeCurrent(device_->display,device_->surface,device_->surface,device_->context))glDeleteBuffers(1,&buffer_);
 }
 size_t size()const override{return bytes_;}
 bool compatible(const std::shared_ptr<GlesDevice>& device)const{return device_.get()==device.get();}
 GLuint buffer()const{return buffer_;}
 bool read(std::span<uint8_t> output)const override {
  if(output.size()<bytes_)return false;
  std::lock_guard lock(device_->mutex);RestoreContext restore;
  if(!eglMakeCurrent(device_->display,device_->surface,device_->surface,device_->context))return false;
  for(unsigned n=0;n<16&&glGetError()!=GL_NO_ERROR;++n){}
  glBindBuffer(GL_COPY_READ_BUFFER,buffer_);
  const void* mapped=glMapBufferRange(GL_COPY_READ_BUFFER,0,static_cast<GLsizeiptr>(bytes_),GL_MAP_READ_BIT);
  if(!mapped)return false;
  std::vector<uint8_t> temporary(bytes_);std::memcpy(temporary.data(),mapped,bytes_);
  if(glUnmapBuffer(GL_COPY_READ_BUFFER)!=GL_TRUE||glGetError()!=GL_NO_ERROR)return false;
  std::memcpy(output.data(),temporary.data(),bytes_);return true;
 }
private:
 std::shared_ptr<GlesDevice> device_;
 GLuint buffer_=0;
 size_t bytes_=0;
};
}
struct GpuPixelKernels::Impl {std::shared_ptr<GlesDevice> device=std::make_shared<GlesDevice>();};
GpuPixelKernels::GpuPixelKernels(bool enabled):impl_(std::make_unique<Impl>()){
 if(!enabled)return;RestoreContext restore;
 impl_->device->display=eglGetDisplay(EGL_DEFAULT_DISPLAY);EGLint major=0,minor=0;if(impl_->device->display==EGL_NO_DISPLAY||!eglInitialize(impl_->device->display,&major,&minor))return;
 EGLint attrs[]={EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_ES3_BIT,EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_NONE};EGLConfig config;EGLint count=0;if(!eglChooseConfig(impl_->device->display,attrs,&config,1,&count)||!count)return;
 EGLint contextAttrs[]={EGL_CONTEXT_CLIENT_VERSION,3,EGL_NONE},surfaceAttrs[]={EGL_WIDTH,1,EGL_HEIGHT,1,EGL_NONE};impl_->device->surface=eglCreatePbufferSurface(impl_->device->display,config,surfaceAttrs);impl_->device->context=eglCreateContext(impl_->device->display,config,EGL_NO_CONTEXT,contextAttrs);
 if(impl_->device->surface==EGL_NO_SURFACE||impl_->device->context==EGL_NO_CONTEXT||!eglMakeCurrent(impl_->device->display,impl_->device->surface,impl_->device->surface,impl_->device->context))return;
 GLint glMajor=0,glMinor=0;glGetIntegerv(GL_MAJOR_VERSION,&glMajor);glGetIntegerv(GL_MINOR_VERSION,&glMinor);if(glMajor<3||(glMajor==3&&glMinor<1)){std::cerr<<"warning: ANDROID_GPU_UNAVAILABLE OpenGL ES 3.1 compute required; CPU composition selected\n";return;}
 impl_->device->program=make_compute_program(compute_source,"pixels");
 if(!impl_->device->program)return;
 impl_->device->blur_program=make_compute_program(effector_blur_source,"effector_blur");
 impl_->device->displacement_program=make_compute_program(effector_displacement_source,"effector_displacement");
 impl_->device->vector_program=make_compute_program(effector_vector_source,"effector_vector");
 impl_->device->transform_program=make_compute_program(effector_transform_source,"effector_transform");
 impl_->device->temporal_program=make_compute_program(effector_temporal_source,"effector_temporal");
 glGenBuffers(5,impl_->device->buffers);if(auto name=glGetString(GL_RENDERER))impl_->device->name=reinterpret_cast<const char*>(name);
}
std::shared_ptr<GpuPixelStorage> GpuPixelKernels::store_pixels(std::span<const uint8_t> pixels){
 if(!available()||pixels.empty()||pixels.size()>size_t(std::numeric_limits<GLsizeiptr>::max())-3)return {};
 auto device=impl_->device;std::lock_guard lock(device->mutex);RestoreContext restore;
 if(!eglMakeCurrent(device->display,device->surface,device->surface,device->context))return {};
 for(unsigned n=0;n<16&&glGetError()!=GL_NO_ERROR;++n){}
 GLuint buffer=0;glGenBuffers(1,&buffer);if(!buffer)return {};
 glBindBuffer(GL_COPY_WRITE_BUFFER,buffer);
 const size_t capacity=(pixels.size()+3)&~size_t(3);
 glBufferData(GL_COPY_WRITE_BUFFER,static_cast<GLsizeiptr>(capacity),nullptr,GL_STATIC_DRAW);
 glBufferSubData(GL_COPY_WRITE_BUFFER,0,static_cast<GLsizeiptr>(pixels.size()),pixels.data());
 if(glGetError()!=GL_NO_ERROR){glDeleteBuffers(1,&buffer);return {};}
 // Only driver-owned GPU storage survives this call; no CPU pixel copy.
 return std::make_shared<GlesPixelStorage>(std::move(device),buffer,pixels.size());
}
GpuPixelKernels::~GpuPixelKernels()=default;
bool GpuPixelKernels::available()const{return impl_->device->program!=0;}
std::string GpuPixelKernels::device_name()const{return impl_->device->name;}
PixelKernelResult GpuPixelKernels::scaled_alpha(const ScaledAlphaPixels& p){
 auto fallback=[&](){return PixelKernelResult{compose_scaled_alpha_bgra(p.destination,p.destination_width,p.destination_height,p.destination_stride,p.source,p.source_width,p.source_height,p.source_stride,p.x_q16,p.y_q16,p.origin_x_q16,p.origin_y_q16,p.scale_x_q16,p.scale_y_q16,p.parameter,p.filtered),false};};
 if(!available()||p.scale_x_q16<=32768||p.scale_y_q16<=32768||p.parameter>256||p.source_width>32767||p.source_height>32767||p.destination_width>32767||p.destination_height>32767||p.destination_stride<uint64_t(p.destination_width)*4||p.source_stride<uint64_t(p.source_width)*4||p.destination_stride%4||p.source_stride%4||p.source_stride>32764||p.destination.size()<uint64_t(p.destination_stride)*p.destination_height||p.source.size()<uint64_t(p.source_stride)*p.source_height||overlap(p.destination,p.source))return fallback();
 std::vector<uint32_t> a(64);a[0]=0;a[1]=p.destination_width;a[2]=p.destination_height;a[3]=p.destination_stride;a[4]=p.source_stride;a[10]=p.parameter;
 double ix=65536.0/double(p.scale_x_q16),iy=65536.0/double(p.scale_y_q16);volatile double tx=double(p.x_q16)/65536.0*ix,ty=double(p.y_q16)/65536.0*iy;
 a[16]=uint32_t(truncate_sse((double(p.origin_x_q16)/65536.0-tx)*65536.0))+(p.filtered?0u:32768u);a[17]=uint32_t(truncate_sse((double(p.origin_y_q16)/65536.0-ty)*65536.0))+(p.filtered?0u:32768u);a[18]=uint32_t(truncate_sse(ix*65536.0));a[25]=uint32_t(truncate_sse(iy*65536.0));a[20]=p.source_width;a[21]=p.source_height;a[22]=p.filtered;
 auto r=impl_->device->dispatch(p.destination,p.source,{},a);return r.success?r:fallback();
}
PixelKernelResult GpuPixelKernels::crossfade(const CrossfadePixels& p){
 auto fallback=[&](){return PixelKernelResult{blend_original_sprite_crossfade(p.destination,p.first,p.second,p.mix,p.opacity),false};};
 uint32_t w=std::min(p.first.width,p.second.width),h=std::min(p.first.height,p.second.height);
 if(!available()||!valid(p.destination)||!valid(p.first)||!valid(p.second)||p.destination.format!=1||p.first.format!=2||p.second.format!=2||p.destination.width<w||p.destination.height<h||p.mix<0||p.mix>256||p.opacity>=256||overlap(p.destination.pixels,p.first.pixels)||overlap(p.destination.pixels,p.second.pixels))return fallback();
 std::vector<uint32_t>a(64);a[0]=1;a[1]=w;a[2]=h;a[3]=p.destination.stride;a[4]=p.first.stride;a[5]=p.second.stride;a[6]=p.destination.byte_offset;a[7]=p.first.byte_offset;a[8]=p.second.byte_offset;a[10]=p.opacity;a[11]=uint32_t(p.mix);auto r=impl_->device->dispatch(p.destination.pixels,p.first.pixels,p.second.pixels,a,true);return r.success?r:fallback();
}
PixelKernelResult GpuPixelKernels::affine(const AffinePixels& p){
 auto storage=std::dynamic_pointer_cast<GlesPixelStorage>(p.source_storage);
 if(p.source_storage&&(!storage||!storage->compatible(impl_->device)||!p.source.empty()))return {};
 if(!available()||!p.destination_width||!p.destination_height||!p.source_width||!p.source_height||p.parameter>256||p.destination_width>32767||p.destination_height>32767||p.source_width>32767||p.source_height>32767||p.destination_stride<uint64_t(p.destination_width)*4||p.source_stride<uint64_t(p.source_width)*4||p.destination_stride%4||p.source_stride%4||p.destination.size()<uint64_t(p.destination_stride)*p.destination_height||(storage?storage->size():p.source.size())<uint64_t(p.source_stride)*p.source_height||(!storage&&overlap(p.destination,p.source)))return {};
 std::vector<uint32_t>a(64);a[0]=2;a[1]=p.destination_width;a[2]=p.destination_height;a[3]=p.destination_stride;a[4]=p.source_stride;a[10]=p.parameter;for(unsigned i=0;i<6;++i)a[16+i]=uint32_t(p.matrix[i]);a[24]=a[20];a[25]=a[21];a[20]=p.source_width;a[21]=p.source_height;a[22]=p.filtered;a[23]=p.blend_alpha;return impl_->device->dispatch(p.destination,p.source,{},a,false,true,storage?storage->buffer():0);
}
PixelKernelResult GpuPixelKernels::layer_composite(const LayerCompositePixels& p){
 auto storage=std::dynamic_pointer_cast<GlesPixelStorage>(p.source_storage);
 if(p.source_storage&&(!storage||!storage->compatible(impl_->device)||!p.source.pixels.empty()))return {};
 if(!available()||!valid(p.destination)||!valid(p.source,4,storage?storage->size():0)||p.source.format!=2||(p.destination.format!=1&&p.destination.format!=2)||p.destination.width!=p.source.width||p.destination.height!=p.source.height||p.reciprocal_corrections.size()!=65536||(!storage&&overlap(p.destination.pixels,p.source.pixels)))return {};
 std::vector<uint32_t>a(64);a[0]=3;a[1]=p.destination.width;a[2]=p.destination.height;a[3]=p.destination.stride;a[4]=p.source.stride;a[6]=p.destination.byte_offset;a[7]=p.source.byte_offset;a[9]=p.destination.format;return impl_->device->dispatch(p.destination.pixels,p.source.pixels,p.reciprocal_corrections,a,true,false,storage?storage->buffer():0);
}
PixelKernelResult GpuPixelKernels::effector_blur(const EffectorBlurPixels& p){
 if(!available()||!valid_effector_blur(p)||!valid(p.destination)||!valid(p.source))return {};
 const auto& dst=p.destination;const auto& src=p.source;
 if(p.radius==0){
  if(dst.pixels.data()==src.pixels.data()&&dst.byte_offset==src.byte_offset&&dst.stride==src.stride)return {true,false};
  // CPU radius0 is row-by-row memmove; uploading all overlapping source rows
  // first would change its cross-row alias semantics. Preserve the CPU fallback.
  if(overlap(dst.pixels,src.pixels))return {};
  std::vector<uint32_t>a(64);a[0]=5;a[1]=dst.width;a[2]=dst.height;a[3]=dst.stride;a[4]=src.stride;a[6]=dst.byte_offset;a[7]=src.byte_offset;a[10]=0;a[13]=dst.format;a[14]=src.format;
  return impl_->device->dispatch(dst.pixels,src.pixels,{},a);
 }
 if(p.selector<4&&overlap(dst.pixels,src.pixels))return {};
 const uint32_t radius=std::min(p.radius,255U),diameter=radius*2+1;
 std::vector<uint32_t>a(64);a[1]=dst.width;a[2]=dst.height;a[3]=dst.stride;a[4]=src.stride;a[6]=dst.byte_offset;a[7]=src.byte_offset;
 a[10]=radius;a[11]=((0x20000U+diameter*2)/diameter)&0xffffU;a[12]=p.selector;a[20]=src.width;a[21]=src.height;
 return impl_->device->dispatch(dst.pixels,src.pixels,{},a,false,false,0,true);
}
PixelKernelResult GpuPixelKernels::layer_composite_batch(std::span<const LayerCompositePixels> layers) {
 PixelKernelResult result;
 if(!available()||!valid_layer_composite_batch(layers))return result;
 auto device=impl_->device;
 std::vector<std::shared_ptr<GlesPixelStorage>> sources;sources.reserve(layers.size());
 for(const auto& p:layers){auto source=std::dynamic_pointer_cast<GlesPixelStorage>(p.source_storage);if(!source||!source->compatible(device))return result;sources.push_back(std::move(source));}
 std::lock_guard lock(device->mutex);RestoreContext restore;
 if(!eglMakeCurrent(device->display,device->surface,device->surface,device->context))return result;
 const bool timing=debug_enabled();auto start=timing?Clock::now():Clock::time_point{};
 for(unsigned n=0;n<16&&glGetError()!=GL_NO_ERROR;++n){}
 GLint maximum=0,alignment=0;glGetIntegerv(GL_MAX_SHADER_STORAGE_BLOCK_SIZE,&maximum);glGetIntegerv(GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT,&alignment);
 const auto& first=layers.front();
 if(alignment<=0||maximum<65536||first.destination.pixels.size()>size_t(maximum))return result;
 for(const auto& s:sources)if(s->size()>size_t(maximum))return result;
 const size_t arg_stride=((256+size_t(alignment)-1)/size_t(alignment))*size_t(alignment);
 std::vector<uint8_t> arguments(arg_stride*layers.size());
 for(size_t i=0;i<layers.size();++i){
  const auto& p=layers[i];uint32_t a[64]{};a[0]=3;a[1]=p.destination.width;a[2]=p.destination.height;
  a[3]=p.destination.stride;a[4]=p.source.stride;a[6]=p.destination.byte_offset;a[7]=p.source.byte_offset;a[9]=p.destination.format;
  std::memcpy(arguments.data()+i*arg_stride,a,sizeof(a));
 }
 const auto upload=timing?Clock::now():Clock::time_point{};
 const auto put=[&](unsigned index,std::span<const uint8_t> bytes){
  glBindBuffer(GL_SHADER_STORAGE_BUFFER,device->buffers[index]);
  if(device->capacities[index]<bytes.size()){glBufferData(GL_SHADER_STORAGE_BUFFER,static_cast<GLsizeiptr>(bytes.size()),nullptr,GL_DYNAMIC_COPY);device->capacities[index]=bytes.size();}
  glBufferSubData(GL_SHADER_STORAGE_BUFFER,0,static_cast<GLsizeiptr>(bytes.size()),bytes.data());
  glBindBufferBase(GL_SHADER_STORAGE_BUFFER,index,device->buffers[index]);
 };
 put(0,first.destination.pixels);put(2,first.reciprocal_corrections);put(3,arguments);
 if(timing)result.upload_ns=std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-upload).count();
 if(glGetError()!=GL_NO_ERROR)return result;
 const auto wait=timing?Clock::now():Clock::time_point{};glUseProgram(device->program);
 for(size_t i=0;i<layers.size();++i){
  glBindBufferBase(GL_SHADER_STORAGE_BUFFER,1,sources[i]->buffer());
  glBindBufferRange(GL_SHADER_STORAGE_BUFFER,3,device->buffers[3],static_cast<GLintptr>(i*arg_stride),256);
  glDispatchCompute(((layers[i].destination.width+1)/2+15)/16,(layers[i].destination.height+7)/8,1);
  glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
 }
 glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
 GLsync fence=glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE,0);if(!fence)return result;
 GLenum status=glClientWaitSync(fence,GL_SYNC_FLUSH_COMMANDS_BIT,1000000000ULL);glDeleteSync(fence);
 if(timing)result.wait_ns=std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-wait).count();
 if((status!=GL_ALREADY_SIGNALED&&status!=GL_CONDITION_SATISFIED)||glGetError()!=GL_NO_ERROR)return result;
 const auto download=timing?Clock::now():Clock::time_point{};
 glBindBuffer(GL_SHADER_STORAGE_BUFFER,device->buffers[0]);
 const void* mapped=glMapBufferRange(GL_SHADER_STORAGE_BUFFER,0,static_cast<GLsizeiptr>(first.destination.pixels.size()),GL_MAP_READ_BIT);if(!mapped)return result;
 std::vector<uint8_t> readback(first.destination.pixels.size());std::memcpy(readback.data(),mapped,readback.size());
 if(glUnmapBuffer(GL_SHADER_STORAGE_BUFFER)!=GL_TRUE||glGetError()!=GL_NO_ERROR)return result;
 std::memcpy(first.destination.pixels.data(),readback.data(),readback.size());
 if(timing){result.download_ns=std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-download).count();result.total_ns=std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-start).count();}
 result.success=true;result.used_gpu=true;return result;
}
PixelKernelResult GpuPixelKernels::effector_displacement(const EffectorDisplacementPixels& p){
 if(!available()||!impl_->device->displacement_program||!valid_effector_displacement(p)||
  !valid(p.destination)||!valid(p.source)||!valid(p.first)||(p.second&&!valid(*p.second)))return {};
 std::vector<uint32_t>a(64);a[1]=p.destination.width;a[2]=p.destination.height;a[3]=p.destination.stride;
 a[4]=p.source.stride;a[5]=p.first.stride;a[6]=p.destination.byte_offset;a[7]=p.source.byte_offset;a[8]=p.first.byte_offset;
 a[20]=p.source.width;a[21]=p.source.height;
 a[10]=p.strength;a[11]=p.bilinear;a[12]=p.second.has_value();if(p.second){a[13]=p.second->stride;a[14]=p.second->byte_offset;}
 return impl_->device->dispatch(p.destination.pixels,p.source.pixels,p.first.pixels,a,false,false,0,false,
  impl_->device->displacement_program,p.second?p.second->pixels:std::span<const uint8_t>{});
}
PixelKernelResult GpuPixelKernels::effector_vector(const EffectorVectorPixels& p){
 if(!available()||!impl_->device->vector_program||!valid_effector_vector(p)||
  !valid(p.destination)||!valid(p.source)||!valid(p.normal,6))return {};
 std::vector<uint32_t>a(64);a[1]=p.destination.width;a[2]=p.destination.height;a[3]=p.destination.stride;
 a[4]=p.source.stride;a[5]=p.normal.stride;a[6]=p.destination.byte_offset;a[7]=p.source.byte_offset;a[8]=p.normal.byte_offset;
 a[10]=p.bilinear;a[11]=p.rectangular_bounds;a[12]=static_cast<uint32_t>(p.coefficients.size());a[14]=(p.source.stride/4)*p.source.height;
 const auto coefficients=std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(p.coefficients.data()),p.coefficients.size_bytes());
 return impl_->device->dispatch(p.destination.pixels,p.source.pixels,p.normal.pixels,a,false,false,0,false,
  impl_->device->vector_program,coefficients);
}
PixelKernelResult GpuPixelKernels::effector_transform(const EffectorTransformPixels& p){
 if(!available()||!impl_->device->transform_program||!valid_effector_transform(p)||!valid(p.destination)||!valid(p.source)||
  (!p.bilinear&&(p.source.width>32767||p.source.height>32767||p.source.stride>131068)))return {};
 std::vector<uint32_t>a(64);a[1]=p.destination.width;a[2]=p.destination.height;a[3]=p.destination.stride;a[4]=p.source.stride;
 a[6]=p.destination.byte_offset;a[7]=p.source.byte_offset;a[10]=p.bilinear;a[12]=p.source.width;a[13]=p.source.height;
 for(unsigned i=0;i<6;++i)a[16+i]=uint32_t(p.matrix[i]);
 return impl_->device->dispatch(p.destination.pixels,p.source.pixels,{},a,false,false,0,false,impl_->device->transform_program);
}
PixelKernelResult GpuPixelKernels::effector_temporal(const EffectorTemporalPixels& p){
 if(!available()||!impl_->device->temporal_program||!valid_effector_temporal(p)||!valid(p.scene)||!valid(p.history)||p.history.stride<uint64_t(p.scene.width)*4)return {};
 std::vector<uint32_t>a(64);a[1]=p.scene.width;a[2]=p.scene.height;a[3]=p.scene.stride;a[4]=p.history.stride;
 a[6]=p.scene.byte_offset;a[7]=p.history.byte_offset;a[10]=p.strength;a[11]=p.scene.format;a[12]=p.history.width;a[13]=p.history.height;
 const auto& table=detail::kRcpSsEstimateTable;const auto bytes=std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(table.data()),sizeof(table));
 return impl_->device->dispatch(p.scene.pixels,p.history.pixels,{},a,true,false,0,false,impl_->device->temporal_program,bytes,p.history.pixels);
}
PixelKernelResult GpuPixelKernels::background_mask(const BackgroundMaskPixels& p){
 if(!available()||!valid(p.destination)||!valid(p.source)||!valid(p.mask,1)||p.destination.format!=1||p.source.format!=1||p.mask.format!=3||p.left>=p.right||p.top>=p.bottom||p.right>p.destination.width||p.bottom>p.destination.height||uint64_t(p.source_left)+p.right-p.left>p.source.width||uint64_t(p.source_top)+p.bottom-p.top>p.source.height||uint64_t(p.mask_left)+p.right-p.left>p.mask.width||uint64_t(p.mask_top)+p.bottom-p.top>p.mask.height||(p.shift!=7&&p.shift!=12)||overlap(p.destination.pixels,p.source.pixels)||overlap(p.destination.pixels,p.mask.pixels))return {};
 for(auto f:p.factors)if(f< -1||f>32767)return {};
 std::vector<uint32_t>a(320);a[0]=4;a[1]=p.right-p.left;a[2]=p.bottom-p.top;a[3]=p.destination.stride;a[4]=p.source.stride;a[5]=p.mask.stride;a[6]=p.destination.byte_offset;a[7]=p.source.byte_offset;a[8]=p.mask.byte_offset;a[26]=p.left;a[27]=p.top;a[28]=p.source_top;a[29]=p.mask_left;a[30]=p.mask_top;a[31]=p.source_left;a[32]=p.shift;for(unsigned i=0;i<256;++i)a[64+i]=uint32_t(p.factors[i]);return impl_->device->dispatch(p.destination.pixels,p.source.pixels,p.mask.pixels,a);
}
PixelKernelResult GpuPixelKernels::pixel_transfer(const PixelTransferPixels& p){
 auto storage=std::dynamic_pointer_cast<GlesPixelStorage>(p.source_storage);
 if(p.source_storage&&(!storage||!storage->compatible(impl_->device)||!p.source.pixels.empty()))return {};
 if(!available()||!valid_pixel_transfer(p)||p.destination.byte_offset%4||p.destination.stride%4)return {};
 const auto op=static_cast<uint32_t>(p.operation);if(op!=3&&op!=5&&(p.source.byte_offset%4||p.source.stride%4))return {};
 std::vector<uint32_t>a(64);a[0]=5;a[1]=p.destination.width;a[2]=p.destination.height;a[3]=p.destination.stride;a[4]=p.source.stride;a[6]=p.destination.byte_offset;a[7]=p.source.byte_offset;a[10]=op;a[11]=p.parameter;a[12]=p.color;a[13]=p.destination.format;a[14]=p.source.format;return impl_->device->dispatch(p.destination.pixels,p.source.pixels,{},a,false,false,storage?storage->buffer():0);
}
}
#endif
