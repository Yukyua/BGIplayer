#include "bgiplayer/game_window.hpp"
#include "bgiplayer/default_game_icon.hpp"
#include "bgiplayer/engine_identity.hpp"
#include "bgiplayer/host_files.hpp"
#include <SDL.h>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
}
namespace bgiplayer {
namespace {
#include "default_game_icon.inc"
using Bytes=std::span<const uint8_t>;
uint16_t u16(Bytes b,size_t p) { return uint16_t(b[p])|uint16_t(b[p+1]<<8); }
uint32_t u32(Bytes b,size_t p) { return uint32_t(u16(b,p))|(uint32_t(u16(b,p+2))<<16); }
bool fits(Bytes b,size_t p,size_t n) { return p<=b.size() && n<=b.size()-p; }
struct Resources {
    Bytes image, root; size_t sections=0; uint16_t count=0;
    Bytes rva(uint32_t at,uint32_t size) const {
        for(size_t i=0;i<count;++i) {
            size_t s=sections+i*40; uint32_t base=u32(image,s+12), length=u32(image,s+16);
            if(at<base || uint64_t(at-base)+size>length)continue;
            uint64_t raw=uint64_t(u32(image,s+20))+at-base;
            if(raw>image.size() || size>image.size()-raw)return {};
            return image.subspan(size_t(raw),size);
        } return {};
    }
    // Resource trees are fixed type/name/language depth; never follow arbitrary recursion.
    std::vector<std::pair<uint32_t,uint32_t>> entries(uint32_t dir) const {
        std::vector<std::pair<uint32_t,uint32_t>> result;
        if(!fits(root,dir,16))return result;
        size_t n=size_t(u16(root,dir+12))+u16(root,dir+14);
        if(!fits(root,size_t(dir)+16,n*8))return result;
        for(size_t i=0;i<n;++i)result.emplace_back(u32(root,dir+16+i*8),u32(root,dir+20+i*8));
        return result;
    }
    std::vector<std::pair<uint32_t,Bytes>> type(uint32_t wanted) const {
        std::vector<std::pair<uint32_t,Bytes>> out;
        for(auto [id,dir]:entries(0))if(id==wanted && (dir>>31))
            for(auto [name,langs]:entries(dir&0x7fffffff))if(langs>>31)
                for(auto [language,leaf]:entries(langs&0x7fffffff)) {
                    (void)language;
                    if((leaf>>31)||!fits(root,leaf,16))continue;
                    auto data=rva(u32(root,leaf),u32(root,leaf+4));
                    if(!data.empty()) {out.emplace_back(name,data);break;}
                }
        return out;
    }
};
std::optional<GameWindowIcon> decode(Bytes b) {
    GameWindowIcon out;
    if(b.size()>=8 && b[0]==137 && b[1]=='P' && b[2]=='N' && b[3]=='G') {
        auto codec=avcodec_find_decoder(AV_CODEC_ID_PNG);
        auto ctx=std::unique_ptr<AVCodecContext,void(*)(AVCodecContext*)>(avcodec_alloc_context3(codec),[](auto p){avcodec_free_context(&p);});
        auto frame=std::unique_ptr<AVFrame,void(*)(AVFrame*)>(av_frame_alloc(),[](auto p){av_frame_free(&p);});
        auto packet=std::unique_ptr<AVPacket,void(*)(AVPacket*)>(av_packet_alloc(),[](auto p){av_packet_free(&p);});
        if(!ctx||!frame||!packet||b.size()>16*1024*1024||avcodec_open2(ctx.get(),codec,nullptr)<0)return {};
        ctx->max_pixels=1024*1024;
        if(av_new_packet(packet.get(),int(b.size()))<0)return {};
        std::copy(b.begin(),b.end(),packet->data);
        if(avcodec_send_packet(ctx.get(),packet.get())<0||avcodec_receive_frame(ctx.get(),frame.get())<0)return {};
        if(frame->width<=0||frame->height<=0||frame->width>1024||frame->height>1024)return {};
        out.width=uint32_t(frame->width);out.height=uint32_t(frame->height);out.rgba.resize(size_t(out.width)*out.height*4);
        auto sws=sws_getContext(frame->width,frame->height,AVPixelFormat(frame->format),frame->width,frame->height,AV_PIX_FMT_RGBA,SWS_POINT,nullptr,nullptr,nullptr);
        if(!sws)return {};
        uint8_t* dest[]={out.rgba.data()};int stride[]={int(out.width*4)};
        int rows=sws_scale(sws,frame->data,frame->linesize,0,frame->height,dest,stride);sws_freeContext(sws);
        if(rows!=frame->height)return {};
        return out;
    }
    if(b.size()<40)return {};
    uint32_t header=u32(b,0),w=u32(b,4),combined=u32(b,8),compression=u32(b,16);
    uint16_t bits=u16(b,14);
    if(header<40||header>b.size()||w==0||w>1024||combined==0||combined>2048||(combined&1)||u16(b,12)!=1||compression!=0)return {};
    if(bits!=1&&bits!=4&&bits!=8&&bits!=24&&bits!=32)return {};
    uint32_t h=combined/2,palette=bits<=8?(u32(b,32)?u32(b,32):(1U<<bits)):0;
    if(palette>(bits<=8?(1U<<bits):0U))return {};
    size_t pixels=header+size_t(palette)*4,stride=((size_t(w)*bits+31)/32)*4,mask_stride=((size_t(w)+31)/32)*4;
    if(!fits(b,pixels,stride*h))return {};
    size_t mask=pixels+stride*h;bool has_mask=fits(b,mask,mask_stride*h),has_alpha=false;
    if(bits==32)for(uint32_t y=0;y<h;++y)for(uint32_t x=0;x<w;++x)has_alpha|=b[pixels+y*stride+x*4+3]!=0;
    out.width=w;out.height=h;out.rgba.resize(size_t(w)*h*4);
    for(uint32_t y=0;y<h;++y)for(uint32_t x=0;x<w;++x) {
        size_t row=pixels+(h-1-y)*stride,p=row+size_t(x)*(bits/8),d=(size_t(y)*w+x)*4;
        if(bits<=8) {
            uint8_t index=bits==8?b[row+x]:bits==4?uint8_t((b[row+x/2]>>((1-x%2)*4))&15):uint8_t((b[row+x/8]>>(7-x%8))&1);
            if(index>=palette)return {};p=header+size_t(index)*4;
        }
        out.rgba[d]=b[p+2];out.rgba[d+1]=b[p+1];out.rgba[d+2]=b[p];
        out.rgba[d+3]=bits==32&&has_alpha?b[p+3]:255;
        if(has_mask && (b[mask+(h-1-y)*mask_stride+x/8]&(0x80>>(x%8))))out.rgba[d+3]=0;
    }return out;
}
// Optional launcher cache export. Resource decoding remains the same as the window icon.
void export_launcher_icon(const GameWindowIcon& icon) {
    const char* path = std::getenv("BGI_GAME_ICON_OUTPUT");
    if (!path || !*path) return;
    auto codec = avcodec_find_encoder(AV_CODEC_ID_PNG);
    auto ctx = std::unique_ptr<AVCodecContext, void(*)(AVCodecContext*)>(avcodec_alloc_context3(codec), [](auto p){avcodec_free_context(&p);});
    auto frame = std::unique_ptr<AVFrame, void(*)(AVFrame*)>(av_frame_alloc(), [](auto p){av_frame_free(&p);});
    auto packet = std::unique_ptr<AVPacket, void(*)(AVPacket*)>(av_packet_alloc(), [](auto p){av_packet_free(&p);});
    bool ok = false;
    if (codec && ctx && frame && packet) {
        ctx->width = int(icon.width); ctx->height = int(icon.height);
        ctx->pix_fmt = AV_PIX_FMT_RGBA; ctx->time_base = AVRational{1,1};
        frame->width = ctx->width; frame->height = ctx->height; frame->format = ctx->pix_fmt;
        frame->data[0] = const_cast<uint8_t*>(icon.rgba.data()); frame->linesize[0] = int(icon.width * 4);
        if (avcodec_open2(ctx.get(), codec, nullptr) >= 0 && avcodec_send_frame(ctx.get(), frame.get()) >= 0 &&
            avcodec_receive_packet(ctx.get(), packet.get()) >= 0)
            ok = host_files::write(std::filesystem::path(path), Bytes(packet->data, size_t(packet->size)));
    }
    if (!ok) std::cerr << "warning: GAME_ICON_CACHE_WRITE_FAILED\n";
}

}
std::optional<GameWindowIcon> game_icon_from_pe(Bytes b) {
    if(b.size()<64||u16(b,0)!=0x5a4d)return {};
    size_t pe=u32(b,60);if(!fits(b,pe,24)||u32(b,pe)!=0x4550)return {};
    size_t optional=pe+24;uint16_t size=u16(b,pe+20),count=u16(b,pe+6);
    if(!fits(b,optional,size)||size<2)return {};
    size_t directory=u16(b,optional)==0x10b?96:u16(b,optional)==0x20b?112:0;
    if(!directory||size<directory+24||u32(b,optional+directory-4)<3)return {};
    size_t sections=optional+size;if(!fits(b,sections,size_t(count)*40))return {};
    Resources r{b,{},sections,count};r.root=r.rva(u32(b,optional+directory+16),u32(b,optional+directory+20));if(r.root.empty())return {};
    auto icons=r.type(3);auto groups=r.type(14);
    std::optional<GameWindowIcon> best;
    // First resource group is the executable's application icon, not every ancillary icon.
    if(groups.empty())return {};
    Bytes group=groups.front().second;
    if(group.size()<6||u16(group,0)!=0||u16(group,2)!=1||!fits(group,6,size_t(u16(group,4))*14))return {};
    for(size_t i=0;i<u16(group,4);++i) {
        uint16_t id=u16(group,6+i*14+12);
        for(auto [icon_id,data]:icons)if(icon_id==id) {
            auto icon=decode(data);
            if(icon&&(!best||uint64_t(icon->width)*icon->height>uint64_t(best->width)*best->height))best=std::move(icon);
        }
    }return best;
}
std::optional<GameWindowIcon> default_game_window_icon() {
    return decode(default_game_icon_png);
}
void configure_game_window(SDL_Window* window,const std::filesystem::path& game) {
    const auto apply_icon=[window](GameWindowIcon& icon) {
        auto surface=SDL_CreateRGBSurfaceWithFormatFrom(icon.rgba.data(),int(icon.width),int(icon.height),32,int(icon.width*4),SDL_PIXELFORMAT_RGBA32);
        if(!surface)return false;
        SDL_SetWindowIcon(window,surface);SDL_FreeSurface(surface);
#ifdef __APPLE__
        set_game_dock_icon(icon);
#endif
        return true;
    };
    std::error_code error;auto files=host_files::entries(game,false,error);
    std::sort(files.begin(),files.end(),[](const auto& a,const auto& b){return a.path<b.path;});
    for(const auto& file:files) {
        if(!file.regular)continue;
        auto extension=file.path.extension().string(),name=file.path.filename().string();
        auto lower=[](unsigned char c){return char(std::tolower(c));};
        std::transform(extension.begin(),extension.end(),extension.begin(),lower);std::transform(name.begin(),name.end(),name.begin(),lower);
        if(extension!=".exe"||name=="cinderella_cn.exe")continue;
        try {
            auto bytes=host_files::read(file.path);if(!engine_identifier_from_pe(bytes))continue;
            auto icon=game_icon_from_pe(bytes);if(!icon)continue;
            if(apply_icon(*icon)) { export_launcher_icon(*icon); return; }
        }catch(const std::exception&) { /* A damaged optional icon must not prevent the game from opening. */ }
    }
    // The fallback is part of the binary: standalone CLI and packaged engines
    // use exactly the same launcher image without needing a UI asset directory.
    auto fallback=default_game_window_icon();
    if(fallback && apply_icon(*fallback))return;
    std::cerr<<"warning: GAME_WINDOW_ICON_UNAVAILABLE game icon and embedded launcher icon could not be applied\n";
}
}
