#ifdef _WIN32
#include "bgiplayer/host_text_edit_bridge.hpp"
#include <SDL_syswm.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <imm.h>
#include <algorithm>
#include <iostream>
#include <string>

namespace bgiplayer {
namespace {
std::wstring wide(std::string_view text) {
    std::wstring result;
    for (uint32_t cp : decode_text_code_points(text, TextEncoding::Utf8)) {
        if (cp > 0xFFFF) { cp -= 0x10000; result += wchar_t(0xD800+(cp>>10)); result += wchar_t(0xDC00+(cp&1023)); }
        else result += wchar_t(cp);
    }
    return result;
}
std::string utf8(std::wstring_view text) {
    std::string result;
    for (std::size_t i=0;i<text.size();++i) {
        uint32_t cp=text[i];
        if (cp>=0xD800 && cp<=0xDBFF && i+1<text.size() && text[i+1]>=0xDC00 && text[i+1]<=0xDFFF)
            cp=0x10000+((cp-0xD800)<<10)+(text[++i]-0xDC00);
        result += encode_code_point(cp,TextEncoding::Utf8);
    }
    return result;
}
class WindowsTextEdit final : public HostTextEditBridge {
    static constexpr wchar_t property[] = L"BGIplayer.text-edit.bridge";
    HostTextEdit& state_;
    SDL_Renderer* renderer_;
    HWND parent_, edit_ = nullptr;
    WNDPROC parent_proc_ = nullptr, edit_proc_ = nullptr;
    HFONT font_ = nullptr;
    bool composing_ = false;
    wchar_t high_surrogate_ = 0;
    std::string committed_;
    std::string created_face_;
    uint32_t created_size_=0,created_width_=0,created_alignment_=0;
    bool created_multiline_=false;
    uint32_t painted_color_=0;
    int painted_scale_=0;
    std::string text() const {
        if (!edit_) return {};
        std::wstring value(static_cast<std::size_t>(GetWindowTextLengthW(edit_))+1,0);
        const int count=GetWindowTextW(edit_,value.data(),static_cast<int>(value.size()));
        value.resize(std::max(count,0));
        return utf8(value);
    }
    void commit() {
        if (edit_ && !composing_) { committed_=text(); state_.commit_text(committed_); }
    }
    bool permits(std::string_view insertion, bool direct) {
        DWORD first=0,last=0;
        SendMessageW(edit_,EM_GETSEL,reinterpret_cast<WPARAM>(&first),reinterpret_cast<LPARAM>(&last));
        return state_.permits_replacement(text(),first,last-first,insertion,direct);
    }
    static LRESULT CALLBACK parent_proc(HWND hwnd,UINT message,WPARAM w,LPARAM l) {
        auto* self=static_cast<WindowsTextEdit*>(GetPropW(hwnd,property));
        if (!self) return DefWindowProcW(hwnd,message,w,l);
        if (message==WM_CTLCOLOREDIT && reinterpret_cast<HWND>(l)==self->edit_) {
            SetTextColor(reinterpret_cast<HDC>(w),self->state_.color());
            SetBkMode(reinterpret_cast<HDC>(w),TRANSPARENT);
            return reinterpret_cast<LRESULT>(GetStockObject(HOLLOW_BRUSH));
        }
        if (message==WM_COMMAND && reinterpret_cast<HWND>(l)==self->edit_ && HIWORD(w)==EN_CHANGE)
            self->commit();
        return CallWindowProcW(self->parent_proc_,hwnd,message,w,l);
    }
    static LRESULT CALLBACK edit_proc(HWND hwnd,UINT message,WPARAM w,LPARAM l) {
        auto* self=static_cast<WindowsTextEdit*>(GetPropW(hwnd,property));
        if (!self) return DefWindowProcW(hwnd,message,w,l);
        if (message==WM_IME_STARTCOMPOSITION) self->composing_=true;
        if (message==WM_IME_ENDCOMPOSITION) {
            const auto result=CallWindowProcW(self->edit_proc_,hwnd,message,w,l);
            self->composing_=false; self->commit(); return result;
        }
        if (message==WM_KEYDOWN && w==VK_TAB) {
            self->state_.finish_edit(true);
            SetFocus(self->parent_);
        }
        if (message==WM_CHAR) {
            if (w==13) {
                SendMessageW(hwnd,EM_SETSEL,0,-1);
                self->state_.finish_edit(false);
                SetFocus(self->parent_);
                return 0;
            }
            if (w==9 || (w<32 && w!=3 && w!=8 && w!=0x16 && w!=0x18)) return 0;
            if (w>=32) {
                const wchar_t unit=static_cast<wchar_t>(w);
                if (unit>=0xD800 && unit<=0xDBFF) { self->high_surrogate_=unit; return 0; }
                std::wstring value;
                if (self->high_surrogate_) { value+=self->high_surrogate_; self->high_surrogate_=0; }
                value+=unit;
                if (!self->permits(utf8(value),true)) return 0;
                if (value.size()==2) CallWindowProcW(self->edit_proc_,hwnd,message,value[0],l);
            }
        }
        if (message==WM_PASTE && OpenClipboard(hwnd)) {
            bool allowed=true;
            if (HANDLE handle=GetClipboardData(CF_UNICODETEXT)) {
                if (const auto* value=static_cast<const wchar_t*>(GlobalLock(handle))) {
                    allowed=self->permits(utf8(value),false);
                    GlobalUnlock(handle);
                }
            }
            CloseClipboard();
            if (!allowed) return 0;
        }
        return CallWindowProcW(self->edit_proc_,hwnd,message,w,l);
    }
    void destroy() {
        if (edit_) {
            if (GetFocus()==edit_) SetFocus(parent_);
            SetWindowLongPtrW(edit_,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(edit_proc_));
            RemovePropW(edit_,property);
            DestroyWindow(edit_); edit_=nullptr;
        }
        if (font_) { DeleteObject(font_); font_=nullptr; }
        composing_=false; high_surrogate_=0;
    }
    void style() {
        if (!edit_) return;
        int x0,y0,x1,y1;
        SDL_RenderLogicalToWindow(renderer_,0,0,&x0,&y0);
        SDL_RenderLogicalToWindow(renderer_,100,100,&x1,&y1);
        const int size=std::max(1,int(state_.edit_font_size())*std::abs(y1-y0)/100);
        painted_scale_=std::abs(y1-y0);painted_color_=state_.color();
        HFONT font=CreateFontW(-size,(size*int(state_.active_width_percent())/100)>>1,0,0,100,
            FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,
            DEFAULT_QUALITY,DEFAULT_PITCH,wide(state_.font_name()).c_str());
        if (font) {
            SendMessageW(edit_,WM_SETFONT,reinterpret_cast<WPARAM>(font),TRUE);
            if (font_) DeleteObject(font_);
            font_=font;
        }
        InvalidateRect(edit_,nullptr,TRUE);
    }
    void create() {
        destroy();
        if (!state_.is_open()) return;
        created_face_=state_.font_name();created_size_=state_.edit_font_size();
        created_width_=state_.active_width_percent();created_alignment_=state_.active_alignment();
        created_multiline_=state_.height()/state_.edit_font_size()>1;
        const DWORD window_style=WS_CHILD|ES_NOHIDESEL|state_.active_alignment()|
            (state_.height()/state_.edit_font_size()>1 ? ES_MULTILINE : ES_AUTOHSCROLL);
        committed_=state_.window_text().value_or("");
        edit_=CreateWindowExW(0,L"EDIT",wide(committed_).c_str(),window_style,
                             0,0,1,1,parent_,nullptr,GetModuleHandleW(nullptr),nullptr);
        if (!edit_) { std::cerr<<"warning: HOST_TEXT_EDIT_CREATE_FAILED platform=windows error="<<GetLastError()<<'\n'; return; }
        SetPropW(edit_,property,this);
        edit_proc_=reinterpret_cast<WNDPROC>(SetWindowLongPtrW(edit_,GWLP_WNDPROC,
                                                   reinterpret_cast<LONG_PTR>(edit_proc)));
        style(); synchronize();
        SendMessageW(edit_,EM_SETSEL,0,-1);
        if (state_.focus_child() && state_.visible()) SetFocus(edit_);
    }
public:
    WindowsTextEdit(HostTextEdit& state,SDL_Window* window,SDL_Renderer* renderer,HWND parent)
        :state_(state),renderer_(renderer),parent_(parent) {
        (void)window;
        SetPropW(parent_,property,this);
        parent_proc_=reinterpret_cast<WNDPROC>(SetWindowLongPtrW(parent_,GWLP_WNDPROC,
                                                   reinterpret_cast<LONG_PTR>(parent_proc)));
        state_.set_host_observer([this](HostTextEdit::HostChange change) {
            if (change==HostTextEdit::HostChange::Create) create();
            else if (change==HostTextEdit::HostChange::Destroy) destroy();
            else { style(); synchronize(); }
        });
        if (state_.is_open()) create();
    }
    ~WindowsTextEdit() override {
        state_.set_host_observer({}); destroy();
        SetWindowLongPtrW(parent_,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(parent_proc_));
        RemovePropW(parent_,property);
    }
    void synchronize() override {
        if (!state_.is_open()) { destroy(); return; }
        if (!edit_) { create(); return; }
        if (created_face_!=state_.font_name() || created_size_!=state_.edit_font_size() ||
            created_width_!=state_.active_width_percent() ||
            created_alignment_!=state_.active_alignment() ||
            created_multiline_!=(state_.height()/state_.edit_font_size()>1)) {create();return;}
        int x0,y0,x1,y1;
        SDL_RenderLogicalToWindow(renderer_,float(state_.x()),float(state_.y()),&x0,&y0);
        SDL_RenderLogicalToWindow(renderer_,float(state_.x()+state_.width()),
                                 float(state_.y()+state_.height()),&x1,&y1);
        MoveWindow(edit_,x0,y0,x1-x0,y1-y0,TRUE);
        int sx0,sy0,sx1,sy1;
        SDL_RenderLogicalToWindow(renderer_,0,0,&sx0,&sy0);
        SDL_RenderLogicalToWindow(renderer_,100,100,&sx1,&sy1);
        if(painted_color_!=state_.color() || painted_scale_!=std::abs(sy1-sy0))style();
        ShowWindow(edit_,state_.visible()?SW_SHOWNA:SW_HIDE);
        const auto current=state_.window_text().value_or("");
        if (current!=committed_) {
            if (current!=text()) { composing_=false; SetWindowTextW(edit_,wide(current).c_str()); }
            committed_=current;
        } else commit();
    }
    bool handle_event(const SDL_Event& event) override {
        return edit_ && GetFocus()==edit_ && (event.type==SDL_KEYDOWN || event.type==SDL_KEYUP ||
            event.type==SDL_TEXTINPUT || event.type==SDL_TEXTEDITING);
    }
};
}
std::unique_ptr<HostTextEditBridge> make_host_text_edit_bridge(
    HostTextEdit& state,SDL_Window* window,SDL_Renderer* renderer) {
    SDL_SysWMinfo info{}; SDL_VERSION(&info.version);
    if (SDL_GetWindowWMInfo(window,&info) && info.subsystem==SDL_SYSWM_WINDOWS)
        return std::make_unique<WindowsTextEdit>(state,window,renderer,info.info.win.window);
    return make_portable_text_edit_bridge(state,window,renderer);
}
} // namespace bgiplayer
#endif
