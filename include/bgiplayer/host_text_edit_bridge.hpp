#pragma once
#include "bgiplayer/host_text_edit.hpp"
#include <SDL.h>
#include <memory>

namespace bgiplayer {
class HostTextEditBridge {
public:
    virtual ~HostTextEditBridge() = default;
    virtual void synchronize() = 0;
    // True means the edit widget owns this event; do not post a BP shortcut.
    virtual bool handle_event(const SDL_Event&) = 0;
    virtual void render() {}
};
std::unique_ptr<HostTextEditBridge> make_host_text_edit_bridge(
    HostTextEdit&, SDL_Window*, SDL_Renderer*);
std::unique_ptr<HostTextEditBridge> make_portable_text_edit_bridge(
    HostTextEdit&, SDL_Window*, SDL_Renderer*);
} // namespace bgiplayer
