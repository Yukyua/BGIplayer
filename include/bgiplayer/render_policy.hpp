#pragma once
#include <optional>
#include <string>
namespace bgiplayer {
enum class RenderMode { Fast, Exact };
extern RenderMode rendering_mode;
// Host policy; configure before rendering/worker startup, never serialized.
void configure_render_mode(const std::optional<std::string>& requested = std::nullopt);
bool retain_cpu_pixel_copies();
inline bool fast_rendering_enabled() { return rendering_mode==RenderMode::Fast; }
} // namespace bgiplayer
