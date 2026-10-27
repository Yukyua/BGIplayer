#include "bgiplayer/sdl_window_mode.hpp"
#include "bgiplayer/window_policy.hpp"

#include <SDL.h>
#include <algorithm>
#include <limits>
#include <iostream>
#include <set>
#include <string>

#ifndef __APPLE__
#include <SDL_syswm.h>
#endif

namespace bgiplayer {
namespace {
void report_mode_failure(const SdlWindowModeRequest& request, const std::string& reason) {
    const std::string key = std::to_string(request.fullscreen) + ":" +
        std::to_string(request.width) + ":" + std::to_string(request.height) + ":" + reason;
    static std::set<std::string> reported;
    if (reported.insert(key).second) {
        std::cerr << "warning: WINDOW_MODE_APPLY_FAILED fullscreen=" << request.fullscreen
                  << " window=" << request.width << 'x' << request.height
                  << " canvas=" << request.canvas_width << 'x' << request.canvas_height
                  << " reason=" << reason << '\n';
    }
}
}  // namespace
#ifndef __APPLE__
bool host_window_has_modal_dialog(SDL_Window* window) {
#ifdef _WIN32
    SDL_SysWMinfo info{};
    SDL_VERSION(&info.version);
    return window && SDL_GetWindowWMInfo(window, &info) &&
        info.subsystem == SDL_SYSWM_WINDOWS && !IsWindowEnabled(info.info.win.window);
#else
    (void)window;
    // Blocking SDL dialogs do not dispatch the host application event loop.
    return false;
#endif
}
#endif

bool apply_sdl_window_mode(SDL_Window* window, SDL_Renderer* renderer,
                           const SdlWindowModeRequest& request) {
    constexpr auto max = static_cast<uint32_t>(std::numeric_limits<int>::max());
    if (!window || !renderer || request.width == 0 || request.height == 0 ||
        request.canvas_width == 0 || request.canvas_height == 0 ||
        request.width > max || request.height > max ||
        request.canvas_width > max || request.canvas_height > max ||
        host_window_has_modal_dialog(window)) return false;
    if (!host_window_changes_supported()) {
        int canvas_width=0, canvas_height=0;
        SDL_RenderGetLogicalSize(renderer,&canvas_width,&canvas_height);
        if (canvas_width!=0 && canvas_height!=0 &&
            (canvas_width!=static_cast<int>(request.canvas_width) ||
             canvas_height!=static_cast<int>(request.canvas_height))) return false;
        if (SDL_RenderSetLogicalSize(renderer,static_cast<int>(request.canvas_width),
                                    static_cast<int>(request.canvas_height))!=0) return false;
        apply_sdl_present_sync(renderer, request.vsync);
        return true;
    }
    SDL_RendererInfo device{};
    if (SDL_GetRendererInfo(renderer, &device) != 0) {
        report_mode_failure(request, SDL_GetError());
        return false;
    }

    const uint32_t previous_flags = SDL_GetWindowFlags(window);
    int previous_x = 0, previous_y = 0, previous_width = 0, previous_height = 0;
    int previous_canvas_width = 0, previous_canvas_height = 0;
    float previous_scale_x = 1, previous_scale_y = 1;
    SDL_Rect previous_viewport{}, previous_clip{};
    const bool previous_clipping = SDL_RenderIsClipEnabled(renderer) == SDL_TRUE;
    SDL_GetWindowPosition(window, &previous_x, &previous_y);
    SDL_GetWindowSize(window, &previous_width, &previous_height);
    SDL_RenderGetLogicalSize(renderer, &previous_canvas_width, &previous_canvas_height);
    SDL_RenderGetScale(renderer, &previous_scale_x, &previous_scale_y);
    SDL_RenderGetViewport(renderer, &previous_viewport);
    SDL_RenderGetClipRect(renderer, &previous_clip);
    const auto rollback = [&]() {
        const uint32_t fullscreen = previous_flags & SDL_WINDOW_FULLSCREEN_DESKTOP;
        SDL_SetWindowFullscreen(window, fullscreen);
        SDL_SetWindowBordered(window, (previous_flags & SDL_WINDOW_BORDERLESS) ? SDL_FALSE : SDL_TRUE);
        SDL_SetWindowPosition(window, previous_x, previous_y);
        SDL_SetWindowSize(window, previous_width, previous_height);
        SDL_RenderSetLogicalSize(renderer, previous_canvas_width, previous_canvas_height);
        SDL_RenderSetScale(renderer, previous_scale_x, previous_scale_y);
        SDL_RenderSetViewport(renderer, &previous_viewport);
        SDL_RenderSetClipRect(renderer, previous_clipping ? &previous_clip : nullptr);
        int restored_width = 0, restored_height = 0;
        SDL_GetWindowSize(window, &restored_width, &restored_height);
        if ((SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN_DESKTOP) != fullscreen ||
            restored_width != previous_width || restored_height != previous_height) {
            report_mode_failure(request, "host window rollback did not restore its prior mode");
        }
    };
    if (SDL_SetWindowFullscreen(window, request.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0) != 0) {
        report_mode_failure(request, SDL_GetError());
        rollback();
        return false;
    }
    if (request.fullscreen) {
        const int display = SDL_GetWindowDisplayIndex(window);
        SDL_Rect bounds{};
        if (display < 0 || SDL_GetDisplayBounds(display, &bounds) != 0) {
            report_mode_failure(request, SDL_GetError());
            rollback();
            return false;
        }
        SDL_SetWindowPosition(window, bounds.x, bounds.y);
        SDL_SetWindowSize(window, bounds.w, bounds.h);
    } else {
        SDL_SetWindowBordered(window, SDL_TRUE);
        SDL_SetWindowSize(window, static_cast<int>(request.width), static_cast<int>(request.height));
        // Keep the script's window position, but make the restored title bar
        // reachable after leaving fullscreen or changing desktop resolution.
        SDL_Rect usable{};
        int x=0, y=0, top=0, left=0, bottom=0, right=0;
        SDL_GetWindowPosition(window, &x, &y);
        const int display=SDL_GetWindowDisplayIndex(window);
        if (display>=0 && SDL_GetDisplayUsableBounds(display, &usable)==0) {
            (void)SDL_GetWindowBordersSize(window, &top, &left, &bottom, &right);
            x=std::max(usable.x+left, std::min(x, usable.x+usable.w-64));
            y=std::max(usable.y+top, std::min(y, usable.y+usable.h-32));
            SDL_SetWindowPosition(window, x, y);
        }
    }
    if (SDL_RenderSetLogicalSize(renderer, static_cast<int>(request.canvas_width),
                                static_cast<int>(request.canvas_height)) != 0) {
        report_mode_failure(request, SDL_GetError());
        rollback();
        return false;
    }
    // Vsync capability varies by renderer; it is not a mode-transition failure.
    apply_sdl_present_sync(renderer, request.vsync);
    return true;
}

void apply_sdl_present_sync(SDL_Renderer* renderer, bool raster_wait) {
    // Raster waiting belongs to the VM. Window scanout is a separate host
    // concern: keep complete frames synchronized even when a script does not
    // request a raster wait. SDL_RENDER_VSYNC=0 permits diagnostic comparison.
    const bool enabled = raster_wait || SDL_GetHintBoolean(SDL_HINT_RENDER_VSYNC, SDL_TRUE);
    if (SDL_RenderSetVSync(renderer, enabled ? 1 : 0) != 0) {
        static std::set<bool> reported;
        if (reported.insert(enabled).second)
            std::cerr << "warning: PRESENT_VSYNC_UNAVAILABLE requested=" << enabled
                      << " reason=" << SDL_GetError() << '\n';
    }
}
}  // namespace bgiplayer
