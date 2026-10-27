#pragma once
#include <cstdint>

struct SDL_Window;
struct SDL_Renderer;
namespace bgiplayer {
struct SdlWindowModeRequest {
    uint32_t fullscreen;
    uint32_t width;
    uint32_t height;
    uint32_t canvas_width;
    uint32_t canvas_height;
    bool vsync;
};
// Apply one host attempt. Failure restores the prior host window/logical state;
// VM decides whether the original fullscreen-to-windowed fallback is attempted.
bool apply_sdl_window_mode(SDL_Window* window, SDL_Renderer* renderer,
                           const SdlWindowModeRequest& request);
bool host_window_has_modal_dialog(SDL_Window* window);
void apply_sdl_present_sync(SDL_Renderer* renderer, bool raster_wait);
}  // namespace bgiplayer
