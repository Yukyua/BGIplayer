#include "bgiplayer/media_source.hpp"
#include "bgiplayer/common.hpp"
#include <SDL.h>
namespace bgiplayer {
namespace {
std::string media_text_utf8(std::string_view text,TextEncoding encoding){
    std::string result;
    for(const auto code:decode_text_code_points(text,encoding))result+=encode_code_point(code,TextEncoding::Utf8);
    return result;
}
}
std::optional<std::string> probe_host_media_source(const MediaSourceRequest& request,bool allow_dialog){
    MediaProbePrompt prompt;
    if(allow_dialog)prompt=[](bool confirm_quit,const MediaSourceRequest& r){
        const auto caption=media_text_utf8(r.caption,r.encoding);
        const auto message=confirm_quit?std::string("Are you sure you want to quit?"):
            media_text_utf8(r.message,r.encoding);
        SDL_MessageBoxButtonData buttons[2];
        if(confirm_quit){
            buttons[0]={0,6,"Yes"};
            buttons[1]={SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT|SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT,7,"No"};
        }else{
            buttons[0]={SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT,1,"OK"};
            buttons[1]={SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT,2,"Cancel"};
        }
        const SDL_MessageBoxData box{SDL_MESSAGEBOX_INFORMATION,nullptr,caption.c_str(),
            message.c_str(),2,buttons,nullptr};
        int selected=-1;
        if(SDL_ShowMessageBox(&box,&selected)!=0)throw Error("removable-media prompt failed");
        return selected;
    };
    return run_media_source_probe(request,host_removable_volumes,[]{
        SDL_Delay(100);
        if(SDL_WasInit(SDL_INIT_EVENTS)){
            SDL_PumpEvents();
            return SDL_HasEvent(SDL_QUIT)==SDL_FALSE;
        }
        return true;
    },prompt);
}
}
