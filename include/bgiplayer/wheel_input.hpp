#pragma once

#include <SDL_events.h>
#include <SDL_version.h>

namespace bgiplayer {
class VM;

// Host-only normalization; no accumulated detents or persistent VM state.
// Returns true when at least one nonzero axis was delivered.
bool handle_sdl_mouse_wheel(VM& vm, const SDL_MouseWheelEvent& event);
}  // namespace bgiplayer
