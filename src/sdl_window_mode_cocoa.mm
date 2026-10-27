#ifdef __APPLE__
#include "bgiplayer/sdl_window_mode.hpp"
#include <SDL.h>
#include <SDL_syswm.h>
#import <AppKit/AppKit.h>

namespace bgiplayer {
bool host_window_has_modal_dialog(SDL_Window* window) {
    @autoreleasepool {
        if ([NSApp modalWindow] != nil) return true;
        SDL_SysWMinfo info{};
        SDL_VERSION(&info.version);
        return window && SDL_GetWindowWMInfo(window, &info) &&
            info.subsystem == SDL_SYSWM_COCOA &&
            [info.info.cocoa.window attachedSheet] != nil;
    }
}
}  // namespace bgiplayer
#endif
