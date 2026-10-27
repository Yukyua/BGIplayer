#include "bgiplayer/host_text_edit_bridge.hpp"
#include "bgiplayer/font_discovery.hpp"
#include "bgiplayer/glyph_freetype.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <optional>

namespace bgiplayer {
namespace {
std::vector<uint32_t> points(std::string_view s) { return decode_text_code_points(s,TextEncoding::Utf8); }
std::string bytes(std::span<const uint32_t> s) {
    std::string result;
    for (const auto cp:s) result+=encode_code_point(cp,TextEncoding::Utf8);
    return result;
}
uint32_t units(std::span<const uint32_t> s) {
    uint32_t result=0;
    for (const auto cp:s) result+=cp>0xFFFF?2:1;
    return result;
}
class PortableTextEdit final : public HostTextEditBridge {
    HostTextEdit& state_;
    SDL_Renderer* renderer_;
    std::size_t anchor_=0,caret_=0;
    bool focused_=false,dirty_=true,was_open_=false, pointer_capture_=false, font_attempted_=false;
    bool resume_focus_=false;
    std::string composition_,committed_;
    int composition_start_=0,composition_length_=0;
    std::optional<GlyphOutlineProvider> glyphs_;
    SDL_Texture* texture_=nullptr;
    uint32_t color_=0,size_=0,width_percent_=0;
    std::string painted_face_;
    uint32_t painted_alignment_=0,painted_width_=0,painted_height_=0;
    void changed(HostTextEdit::HostChange change) {
        dirty_=true;
        if (change==HostTextEdit::HostChange::Create) {
            committed_=state_.window_text().value_or("");
            anchor_=0;caret_=points(committed_).size();
            focused_=state_.focus_child(); composition_.clear(); glyphs_.reset(); font_attempted_=false;
        }
        if (change==HostTextEdit::HostChange::Destroy) {
            focused_=false;composition_.clear();
        }
        synchronize();
    }
    bool replace(std::string_view insertion,bool direct) {
        auto cp=points(state_.window_text().value_or(""));
        const auto first=std::min({anchor_,caret_,cp.size()});
        const auto last=std::min(std::max(anchor_,caret_),cp.size());
        if (!state_.permits_replacement(bytes(cp),units(std::span(cp).first(first)),
            units(std::span(cp).subspan(first,last-first)),insertion,direct)) return false;
        const auto incoming=points(insertion);
        cp.erase(cp.begin()+static_cast<std::ptrdiff_t>(first),cp.begin()+static_cast<std::ptrdiff_t>(last));
        cp.insert(cp.begin()+static_cast<std::ptrdiff_t>(first),incoming.begin(),incoming.end());
        anchor_=caret_=first+incoming.size();
        committed_=bytes(cp); state_.commit_text(committed_);
        composition_.clear();dirty_=true;return true;
    }
    void finish(bool tab) {
        focused_=false;composition_.clear();state_.finish_edit(tab);SDL_StopTextInput();dirty_=true;
    }
    bool inside(int x,int y) const {
        return x>=int(state_.x()) && y>=int(state_.y()) &&
            x<int(state_.x()+state_.width()) && y<int(state_.y()+state_.height());
    }
    uint32_t advance(uint32_t cp) const {
        const uint32_t base=std::max(1U,state_.edit_font_size()*state_.active_width_percent()/200U);
        return cp<0x80 || (cp>=0xFF61 && cp<0xFFA0) ? base:base*2;
    }
    void rasterize() {
        if (texture_) { SDL_DestroyTexture(texture_);texture_=nullptr; }
        if (!state_.is_open() || !state_.visible()) return;
        if (!font_attempted_ || painted_face_!=state_.font_name() ||
            size_!=state_.edit_font_size() || width_percent_!=state_.active_width_percent()) {
            font_attempted_=true;
            painted_face_=state_.font_name();
            const auto faces=discover_host_font_faces(host_font_search_roots());
            const auto face=select_default_cjk_font(faces,state_.font_name());
            size_=state_.edit_font_size();width_percent_=state_.active_width_percent();
            if (face) glyphs_=make_freetype_glyph_provider({face->file,face->face_index,
                int32_t(size_),int32_t(width_percent_),0,0,0,size_,1});
        }
        const uint32_t w=state_.width(),h=state_.height();
        painted_width_=w;painted_height_=h;painted_alignment_=state_.active_alignment();
        std::vector<uint8_t> pixels(std::size_t(w)*h*4,0);
        auto cp=points(committed_);
        const auto first=std::min({anchor_,caret_,cp.size()});
        const auto last=std::min(std::max(anchor_,caret_),cp.size());
        auto preedit=points(composition_);
        cp.insert(cp.begin()+static_cast<std::ptrdiff_t>(std::min(caret_,cp.size())),preedit.begin(),preedit.end());
        int x=0,y=0;
        uint32_t line_width=0;for(auto c:cp) { if(c=='\r'||c=='\n')break;line_width+=advance(c); }
        if(state_.active_alignment()==1) x=std::max(0,(int(w)-int(line_width))/2);
        if(state_.active_alignment()==2) x=std::max(0,int(w)-int(line_width));
        GlyphFontRequest request{};request.height=int(size_);request.width=int(size_*width_percent_/200U);
        request.weight=100;request.outline_gray_bits=6;
        for(const auto c:points(state_.font_name())) {
            if(c<=0xFFFF) request.face+=char16_t(c);
        }
        for(std::size_t i=0;i<cp.size();++i) {
            const auto c=cp[i];
            if(c=='\r') continue;
            if(c=='\n') {x=0;y+=int(size_);continue;}
            const auto a=advance(c);
            if (x+int(a)>int(w) && state_.height()/size_>1) {x=0;y+=int(size_);}
            const bool selected=focused_ && composition_.empty() && i>=first && i<last;
            if(selected) for(uint32_t yy=0;yy<size_ && y+int(yy)<int(h);++yy)
                for(uint32_t xx=0;xx<a && x+int(xx)<int(w);++xx) {
                    if(x+int(xx)<0 || y+int(yy)<0)continue;
                    const auto at=(std::size_t(y+int(yy))*w+std::size_t(x)+xx)*4;
                    pixels[at]=48;pixels[at+1]=96;pixels[at+2]=192;pixels[at+3]=192;
                }
            if(glyphs_) if(const auto mask=(*glyphs_)(request,c)) {
                for(uint32_t yy=0;yy<mask->height;++yy) for(uint32_t xx=0;xx<mask->width;++xx) {
                    if(x+int(xx)<0 || y+int(yy)<0 || x+int(xx)>=int(w) || y+int(yy)>=int(h)) continue;
                    const auto alpha=mask->alpha[std::size_t(yy)*mask->stride+xx];
                    if(alpha==0)continue;
                    const auto at=(std::size_t(y+int(yy))*w+std::size_t(x)+xx)*4;
                    pixels[at]=state_.color()&255;pixels[at+1]=(state_.color()>>8)&255;
                    pixels[at+2]=(state_.color()>>16)&255;pixels[at+3]=alpha;
                }
            }
            if(!composition_.empty() && i>=caret_ && i<caret_+preedit.size() && y+int(size_)<int(h)) {
                for(uint32_t xx=0;xx<a && x+int(xx)<int(w);++xx) {
                    const auto at=(std::size_t(y+int(size_))*w+std::size_t(x)+xx)*4;
                    pixels[at]=pixels[at+1]=pixels[at+2]=pixels[at+3]=255;
                }
            }
            x+=int(a);
        }
        texture_=SDL_CreateTexture(renderer_,SDL_PIXELFORMAT_RGBA32,SDL_TEXTUREACCESS_STATIC,int(w),int(h));
        if(texture_) {SDL_UpdateTexture(texture_,nullptr,pixels.data(),int(w*4));SDL_SetTextureBlendMode(texture_,SDL_BLENDMODE_BLEND);}
    }
public:
    PortableTextEdit(HostTextEdit& state,SDL_Window* window,SDL_Renderer* renderer)
        :state_(state),renderer_(renderer) {
        (void)window;
        state_.set_host_observer([this](auto change){changed(change);});
        if(state_.is_open())changed(HostTextEdit::HostChange::Create);
    }
    ~PortableTextEdit() override {
        state_.set_host_observer({});if(focused_)SDL_StopTextInput();
        if(texture_)SDL_DestroyTexture(texture_);
    }
    void synchronize() override {
        if(!state_.is_open() || !state_.visible()) {focused_=false;composition_.clear();}
        if(focused_) {
            if(!SDL_IsTextInputActive())SDL_StartTextInput();
            int x,y,x1,y1;SDL_RenderLogicalToWindow(renderer_,state_.x(),state_.y(),&x,&y);
            SDL_RenderLogicalToWindow(renderer_,state_.x()+state_.width(),state_.y()+state_.height(),&x1,&y1);
            SDL_Rect rect{x,y,x1-x,y1-y};SDL_SetTextInputRect(&rect);
        } else if(was_open_)SDL_StopTextInput();
        was_open_=state_.is_open();
        const auto text=state_.window_text().value_or("");
        if(text!=committed_) {committed_=text;anchor_=0;caret_=points(text).size();composition_.clear();dirty_=true;}
        if(color_!=state_.color()) {color_=state_.color();dirty_=true;}
        if(painted_face_!=state_.font_name() || size_!=state_.edit_font_size() ||
            width_percent_!=state_.active_width_percent() ||
            painted_alignment_!=state_.active_alignment() || painted_width_!=state_.width() ||
            painted_height_!=state_.height())dirty_=true;
    }
    bool handle_event(const SDL_Event& e) override {
        if(!state_.is_open() || !state_.visible())return false;
        if(e.type==SDL_WINDOWEVENT && e.window.event==SDL_WINDOWEVENT_FOCUS_LOST) {
            resume_focus_=focused_;
            focused_=false;composition_.clear();SDL_StopTextInput();return false;
        }
        if(e.type==SDL_WINDOWEVENT && e.window.event==SDL_WINDOWEVENT_FOCUS_GAINED) {
            focused_=resume_focus_;resume_focus_=false;synchronize();return false;
        }
        if(e.type==SDL_MOUSEMOTION &&
            (pointer_capture_ || inside(e.motion.x,e.motion.y)))return true;
        if(e.type==SDL_MOUSEWHEEL) {
            int x,y;SDL_GetMouseState(&x,&y);float lx,ly;
            SDL_RenderWindowToLogical(renderer_,x,y,&lx,&ly);
            if(inside(int(lx),int(ly)))return true;
        }
        if(e.type==SDL_MOUSEBUTTONUP && e.button.button==SDL_BUTTON_LEFT && pointer_capture_) {
            pointer_capture_=false;return true;
        }
        if(e.type==SDL_MOUSEBUTTONDOWN && e.button.button==SDL_BUTTON_LEFT) {
            focused_=inside(e.button.x,e.button.y);
            if(!focused_){synchronize();return false;}
            const auto cp=points(committed_);int x=int(state_.x());caret_=0;
            while(caret_<cp.size() && x+int(advance(cp[caret_]))/2<e.button.x)x+=int(advance(cp[caret_++]));
            anchor_=caret_;pointer_capture_=true;dirty_=true;synchronize();return true;
        }
        if(!focused_)return false;
        if(e.type==SDL_TEXTEDITING) {
            composition_=e.edit.text;composition_start_=e.edit.start;composition_length_=e.edit.length;
            dirty_=true;return true;
        }
#if SDL_VERSION_ATLEAST(2, 0, 22)
        if(e.type==SDL_TEXTEDITING_EXT) {
            composition_=e.editExt.text ? e.editExt.text : "";
            composition_start_=e.editExt.start;composition_length_=e.editExt.length;
            dirty_=true;return true;
        }
#endif
        if(e.type==SDL_TEXTINPUT) {replace(e.text.text,true);return true;}
        if(e.type==SDL_KEYUP)return true;
        if(e.type!=SDL_KEYDOWN)return false;
        if(!composition_.empty())return true; // IME owns navigation/commit keys.
        const auto key=e.key.keysym.sym;const auto mod=e.key.keysym.mod;
#ifdef __APPLE__
        const bool command=(mod & KMOD_GUI)!=0;
#else
        const bool command=(mod & KMOD_CTRL)!=0;
#endif
        auto cp=points(committed_);
        if(command && key==SDLK_a){anchor_=0;caret_=cp.size();}
        else if(command && (key==SDLK_c || key==SDLK_x)) {
            const auto first=std::min(anchor_,caret_),last=std::max(anchor_,caret_);
            SDL_SetClipboardText(bytes(std::span(cp).subspan(first,last-first)).c_str());
            if(key==SDLK_x)replace("",false);
        } else if(command && key==SDLK_v) {
            if(char* text=SDL_GetClipboardText()){replace(text,false);SDL_free(text);}
        } else if(key==SDLK_RETURN || key==SDLK_KP_ENTER)finish(false);
        else if(key==SDLK_TAB)finish(true);
        else if(key==SDLK_BACKSPACE || key==SDLK_DELETE) {
            if(anchor_==caret_){if(key==SDLK_BACKSPACE && anchor_>0)--anchor_;else if(key==SDLK_DELETE && caret_<cp.size())++caret_;}
            replace("",false);
        } else if(key==SDLK_LEFT || key==SDLK_RIGHT || key==SDLK_HOME || key==SDLK_END) {
            if(key==SDLK_LEFT && caret_>0)--caret_;
            if(key==SDLK_RIGHT && caret_<cp.size())++caret_;
            if(key==SDLK_HOME)caret_=0;if(key==SDLK_END)caret_=cp.size();
            if(!(mod&KMOD_SHIFT))anchor_=caret_;
        }
        dirty_=true;return true;
    }
    void render() override {
        if(!state_.is_open() || !state_.visible())return;
        if(dirty_){rasterize();dirty_=false;}
        const SDL_Rect rect{int(state_.x()),int(state_.y()),int(state_.width()),int(state_.height())};
        if(texture_)SDL_RenderCopy(renderer_,texture_,nullptr,&rect);
        if(focused_ && anchor_==caret_ && composition_.empty()) {
            int x=rect.x,y=rect.y;
            const auto cp=points(committed_);
            for(std::size_t i=0;i<std::min(caret_,cp.size());++i) {
                if(cp[i]=='\n'){x=rect.x;y+=int(size_);}
                else if(cp[i]!='\r')x+=int(advance(cp[i]));
            }
            SDL_SetRenderDrawColor(renderer_,Uint8(state_.color()&255),
                Uint8((state_.color()>>8)&255),Uint8((state_.color()>>16)&255),255);
            if(x>=rect.x && x<rect.x+rect.w)
                SDL_RenderDrawLine(renderer_,x,y,x,std::min(y+int(size_),rect.y+rect.h-1));
        }
    }
};
}
std::unique_ptr<HostTextEditBridge> make_portable_text_edit_bridge(HostTextEdit& state,SDL_Window* window,SDL_Renderer* renderer) {
    return std::make_unique<PortableTextEdit>(state,window,renderer);
}
#if !defined(__APPLE__) && !defined(_WIN32)
std::unique_ptr<HostTextEditBridge> make_host_text_edit_bridge(HostTextEdit& state,SDL_Window* window,SDL_Renderer* renderer) {
    return make_portable_text_edit_bridge(state,window,renderer);
}
#endif
} // namespace bgiplayer
