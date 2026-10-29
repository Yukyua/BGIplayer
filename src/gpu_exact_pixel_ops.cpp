#include "bgiplayer/gpu_exact_pixel_ops.hpp"
#include <cstring>
#include <cstdlib>
namespace bgiplayer {
namespace {
bool view_valid(const GlyphBitmapView& v,const std::shared_ptr<GpuPixelStorage>& storage,bool required) {
    if(!v.width || !v.height)return !required;
    const uint64_t size=storage?storage->size():v.pixels.size();
    return v.bytes_per_pixel && v.bytes_per_pixel<=16 && size<=UINT32_MAX &&
        v.stride>=uint64_t(v.width)*v.bytes_per_pixel &&
        uint64_t(v.byte_offset)+uint64_t(v.height-1)*v.stride+uint64_t(v.width)*v.bytes_per_pixel<=size;
}
bool selected(const ExactPixelPixels& p) {
    const char* mode=std::getenv("BGI_GPU");
    const bool forced=mode && std::strcmp(mode,"1")==0;
    if(!forced && p.operation==ExactPixelOperation::VmBlit && p.parameters[0]==65)return false;
    if(!forced && p.operation==ExactPixelOperation::RawCopy && !p.destination_storage && !p.source_storage)return false;
    return exact_pixels_worthwhile(p.destination.width,p.destination.height,bool(p.destination_storage));
}
}
bool exact_pixels_worthwhile(uint32_t width,uint32_t height,bool resident) {
    const char* mode=std::getenv("BGI_GPU");
    if(mode && std::strcmp(mode,"0")==0)return false;
    return (mode && std::strcmp(mode,"1")==0) || uint64_t(width)*height>=(resident?65536U:262144U);
}
namespace { thread_local ExactPixelStatistics statistics; }
ExactPixelStatistics exact_pixel_statistics(){return statistics;}
bool valid_exact_pixels(const ExactPixelPixels& p) {
    const auto& d=p.destination;const auto& a=p.source;const auto& b=p.second;
    const auto op=p.operation;
    if((p.source_storage&&!a.pixels.empty()) || (p.second_storage&&!b.pixels.empty()) ||
       (p.destination_storage&&!d.pixels.empty()&&d.pixels.size()!=p.destination_storage->size()))return false;
    const bool allocation=op==ExactPixelOperation::Fill && p.destination_bytes && d.pixels.empty() && !p.destination_storage;
    if(p.destination_bytes && (!allocation || p.destination_bytes>UINT32_MAX))return false;
    if((!allocation && !view_valid(d,p.destination_storage,true)) ||
            !view_valid(a,p.source_storage,false) || !view_valid(b,p.second_storage,false))return false;
    if(allocation && (!d.width || !d.height || !d.bytes_per_pixel || d.bytes_per_pixel>4 ||
        d.stride!=uint64_t(d.width)*d.bytes_per_pixel || d.byte_offset || uint64_t(d.stride)*d.height!=p.destination_bytes))return false;
    const auto same=[&](const GlyphBitmapView& v){return v.width==d.width && v.height==d.height;};
    const auto color=[](const GlyphBitmapView& v){return (v.format==1 || v.format==2) && v.bytes_per_pixel==4;};
    const auto disjoint=[&](const GlyphBitmapView& v,bool point){
        if(v.pixels.empty() || d.pixels.empty())return true;
        const uintptr_t dp=reinterpret_cast<uintptr_t>(d.pixels.data()),sp=reinterpret_cast<uintptr_t>(v.pixels.data());
        const bool overlap=dp<sp+v.pixels.size() && sp<dp+d.pixels.size();
        return !overlap || (point && dp==sp && d.byte_offset==v.byte_offset && d.stride==v.stride && d.bytes_per_pixel==v.bytes_per_pixel);
    };
    const bool filter=uint32_t(op)>=1 && uint32_t(op)<=6;
    if(!disjoint(a,filter && op!=ExactPixelOperation::FilterMask) || !disjoint(b,false))return false;
    switch(op){
    case ExactPixelOperation::Fill:return d.bytes_per_pixel<=4;
    case ExactPixelOperation::RawCopy:return d.bytes_per_pixel==a.bytes_per_pixel && uint64_t(d.width)+p.parameters[1]<=a.width && uint64_t(d.height)+p.parameters[2]<=a.height;
    case ExactPixelOperation::Flip:return d.bytes_per_pixel==a.bytes_per_pixel && d.format==a.format && same(a) && p.parameters[0]<=1;
    case ExactPixelOperation::FilterRgb555:return d.format==0 && a.format==0 && d.bytes_per_pixel==2 && a.bytes_per_pixel==2 && same(a) && p.parameters[1]<=256;
    case ExactPixelOperation::FilterMask:return d.format==1 && d.bytes_per_pixel==4 && a.format==3 && a.bytes_per_pixel==1 && same(a) && p.parameters[1]<=256;
    case ExactPixelOperation::FilterAddSub:case ExactPixelOperation::FilterLuminance:case ExactPixelOperation::FilterXor:return color(d)&&color(a)&&d.format==a.format&&same(a)&&p.parameters[1]<=256;
    case ExactPixelOperation::FilterBlackAlpha:return color(d)&&color(a)&&d.format==2&&a.format==2&&same(a)&&p.parameters[1]<=256;
    case ExactPixelOperation::SpriteReduction:case ExactPixelOperation::HalfDownsample:return color(d)&&color(a)&&d.format==a.format&&d.width==(a.width+1)/2&&d.height==(a.height+1)/2;
    case ExactPixelOperation::SpritePairMix:return color(d)&&color(a)&&color(b)&&d.format==2&&a.format==2&&b.format==2&&d.width>=std::min(a.width,b.width)&&d.height>=std::min(a.height,b.height)&&p.parameters[0]<=256;
    case ExactPixelOperation::SpriteTerminal:return color(d)&&color(a)&&d.format==1&&a.format==2&&same(a)&&p.parameters[1]<=256&&(p.parameters[0]==2||p.parameters[0]==3||p.parameters[0]==33||p.parameters[0]==34);
    case ExactPixelOperation::IconRotate:return color(d)&&color(a)&&d.format==2&&a.format==2;
    case ExactPixelOperation::Invert:return d.format==3&&d.bytes_per_pixel==1;
    case ExactPixelOperation::ReplaceColor:return color(d);
    case ExactPixelOperation::Grayscale:return d.format==3&&d.bytes_per_pixel==1&&color(a)&&same(a);
    case ExactPixelOperation::ScaledKernel:return same(a)&&d.format==a.format&&d.bytes_per_pixel==a.bytes_per_pixel&&p.parameters[0]<=1&&p.parameters[1]<=256&&(p.parameters[1]==0||color(d));
    case ExactPixelOperation::ComposeAlpha:return d.format==3&&d.bytes_per_pixel==1&&color(a)&&a.format==2&&p.parameters[2]<=1&&p.parameters[3]<=256&&(!p.parameters[2]||(color(b)&&b.format==2&&b.stride==a.stride));
    case ExactPixelOperation::Expand:return d.format==3&&d.bytes_per_pixel==1&&color(a)&&a.format==2&&p.parameters[0]>=1&&p.parameters[0]<=3;
    case ExactPixelOperation::Resize:return color(d)&&color(a)&&d.format==a.format&&p.parameters[0]<=3&&p.parameters[5]<=p.parameters[7]&&p.parameters[6]<=p.parameters[8]&&p.parameters[7]<=d.width&&p.parameters[8]<=d.height;
    case ExactPixelOperation::MaskedBlend:
        if(!p.parameters[0]) {if(static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(p.parameters[1])))!=p.parameters[1])return false;}
        else for(size_t i=16;i<272;++i){const uint32_t v=p.parameters[i];if(v!=65536&&v!=65537&&static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(v)))!=v)return false;}
        return color(d)&&color(a)&&same(a)&&d.format==a.format&&p.parameters[0]<=1&&(!p.parameters[0]||(d.format==1&&b.format==3&&b.bytes_per_pixel==1&&same(b)));
    case ExactPixelOperation::VmClearMirror:return d.format==2&&d.bytes_per_pixel==4&&same(a)&&p.parameters[1]<=256&&(p.parameters[0]==64||p.parameters[0]==65);
    case ExactPixelOperation::VmBlit: {
        if(!same(a) || p.parameters[1]>256)return false;
        const uint32_t mode=p.parameters[0],parameter=p.parameters[1],sf=a.format,df=d.format;
        const bool dcolor=color(d),acolor=color(a),packed=d.bytes_per_pixel==2&&a.bytes_per_pixel==2&&sf==0&&df==0;
        if(mode==255)return parameter<4&&acolor&&d.bytes_per_pixel>=4;
        if(mode==128)return a.format==3&&a.bytes_per_pixel==1&&d.format==2&&d.bytes_per_pixel==4;
        if(mode==0)return packed;
        if(mode==65)return true;
        if(mode==64)return packed||(acolor&&dcolor);
        if(mode==193)return parameter>0&&parameter<256&&((packed)||(acolor&&dcolor&&sf==df));
        if(mode==240)return parameter>0&&parameter<256&&(packed||(acolor&&dcolor));
        if(mode==8||mode==9||mode==38||mode==39)return acolor&&dcolor&&df==1;
        if(mode==7||mode==37)return acolor&&sf==2&&(dcolor||(df==3&&d.bytes_per_pixel==1));
        const uint32_t ep=mode>=33&&mode<=39?256-parameter:parameter;
        if(mode==6||mode==36)return ep>0&&acolor&&dcolor&&df==1;
        if(mode==4||mode==35)return ep>0&&((acolor&&dcolor&&(sf==1||df==1))||(sf==3&&df==3&&a.bytes_per_pixel==1&&d.bytes_per_pixel==1));
        if(mode==3||mode==34)return ep>0&&acolor&&dcolor&&sf==2&&df==1;
        if(mode==2||mode==33)return ep>0&&ep<256&&packed;
        return false;
    }
    }
    return false;
}
PixelKernelResult compose_host_exact_pixels(const ExactPixelPixels& p) {
    if(!selected(p) || !valid_exact_pixels(p) || p.destination.pixels.empty())return {};
    const auto result=exact_pixel_detail::execute(p);
    if(result.success && result.used_gpu){++statistics.dispatches;++statistics.operations[static_cast<uint32_t>(p.operation)];}
    return result;
}
std::shared_ptr<GpuPixelStorage> compose_host_resident_exact_pixels(const ExactPixelPixels& p) {
    if(!selected(p) || !valid_exact_pixels(p))return {};
    auto result=exact_pixel_detail::execute_resident(p);
    if(result){++statistics.dispatches;++statistics.operations[static_cast<uint32_t>(p.operation)];}
    return result;
}
namespace exact_pixel_detail {
std::array<uint32_t,536> arguments(const ExactPixelPixels& p) {
    std::array<uint32_t,536> a{};
    a[0]=static_cast<uint32_t>(p.operation);
    const auto put=[&](uint32_t i,const GlyphBitmapView& v) {
        a[i]=v.byte_offset;a[i+1]=v.stride;a[i+2]=v.width;a[i+3]=v.height;
        a[i+4]=v.format;a[i+5]=v.bytes_per_pixel;
    };
    put(1,p.destination);put(7,p.source);put(13,p.second);
    std::copy(p.parameters.begin(),p.parameters.end(),a.begin()+24);
    return a;
}
std::string shader_body() {
    return std::string(R"BGI(
#define OP p[0]
#define DOFF p[1]
#define DS p[2]
#define DW p[3]
#define DH p[4]
#define DF p[5]
#define DB p[6]
#define SOFF p[7]
#define SS p[8]
#define SW p[9]
#define SH p[10]
#define SF p[11]
#define SB p[12]
#define BOFF p[13]
#define BS p[14]
#define BW p[15]
#define BH p[16]
#define BF p[17]
#define BB p[18]
#define P(i) p[24u+uint(i)]
#define MIN(a,b) min(a,b)
#define MAX(a,b) max(a,b)
#define CLAMP(a,b,c) clamp(a,b,c)
if(x>=DW || y>=DH)return;
if(OP==7u) {uint so=SOFF+(y+P(2))*SS+(x+P(1))*SB,di=DOFF+y*DS+x*DB;for(uint c=0u;c<DB;++c)W(di+c,A(so+c));return;}
if(OP==8u) {uint di=DOFF+y*DS+x*DB;for(uint c=0u;c<DB;++c)W(di+c,(P(0)>>(8u*(c&3u)))&255u);return;}
)BGI") +
#include "bgiplayer/gpu_exact_filter_shader.inc"
    +
#include "bgiplayer/gpu_exact_sprite_shader.inc"
    +
#include "bgiplayer/gpu_exact_vm_shader.inc"
    +
#include "bgiplayer/gpu_exact_bitmap_color_shader.inc"
    +
#include "bgiplayer/gpu_exact_bitmap_scale_shader.inc"
    ;
}
#if !defined(__APPLE__) && !defined(__ANDROID__)
PixelKernelResult execute(const ExactPixelPixels&) {return {};}
std::shared_ptr<GpuPixelStorage> execute_resident(const ExactPixelPixels&) {return {};}
#endif
}
}
