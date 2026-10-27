#include "bgiplayer/debug.hpp"
#include <array>
#include <cstdlib>
#include <string_view>

namespace bgiplayer {
namespace {
RuntimeDebugSettings from_environment(bool enabled) {
    RuntimeDebugSettings result;
    const char* mode = std::getenv("BGI_DEBUG");
    result.enabled = enabled || (mode && std::string_view(mode) != "0");
    constexpr std::array names{
        "BGI_TRACE_DISPLAY", "BGI_TRACE_DISPLAY_EVERY_FRAME", "BGI_TRACE_TEXT_TIMING",
        "BGI_TRACE_COMMANDS", "BGI_TRACE_BS_PACKET", "BGI_TRACE_FONTS", "BGI_TRACE_ICON_INPUT",
        "BGI_TRACE_MOVIE", "BGI_TRACE_CONTROL", "BGI_TRACE_CHECKS", "BGI_TRACE_GPU",
        "BGI_TRACE_GPU_FRAMES"};
    static_assert(names.size() == static_cast<size_t>(DebugTrace::Count));
    for(size_t i=0;i<names.size();++i)
        if(std::getenv(names[i])) result.traces |= 1U << i;
    result.verify_present_cache = std::getenv("BGI_VERIFY_PRESENT_CACHE") != nullptr;
    if(const char* directory=std::getenv("BGI_TRACE_DISPLAY_FRAMES")) {
        result.display_frames_directory=directory;
        result.traces |= 1U << static_cast<uint32_t>(DebugTrace::Display);
    }
    if(result.traces || result.verify_present_cache) result.enabled=true;
    return result;
}
}
RuntimeDebugSettings runtime_debug=from_environment(false);
void configure_runtime_debug(bool enabled) { runtime_debug=from_environment(enabled); }
} // namespace bgiplayer
