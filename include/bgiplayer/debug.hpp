#pragma once

#include <cstdint>
#include <string>

namespace bgiplayer {
enum class DebugTrace : uint32_t {
    Display, DisplayEveryFrame, TextTiming, Commands, BsPacket, Fonts,
    IconInput, Movie, Control, Checks, Gpu, GpuFrames, Count
};
// Host diagnostics only. Configure at startup, before worker threads run.
// Warnings and fatal errors never depend on this configuration.
struct RuntimeDebugSettings {
    bool enabled = false;
    uint32_t traces = 0;
    bool verify_present_cache = false;
    std::string display_frames_directory;
};
extern RuntimeDebugSettings runtime_debug;
void configure_runtime_debug(bool enabled = false);
inline bool debug_enabled() { return runtime_debug.enabled; }
inline bool debug_trace(DebugTrace trace) {
    return (runtime_debug.traces & (1U << static_cast<uint32_t>(trace))) != 0;
}
} // namespace bgiplayer
