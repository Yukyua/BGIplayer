#include "bgiplayer/render_policy.hpp"
#include "bgiplayer/common.hpp"
#include <cstdlib>
#include <string_view>
namespace bgiplayer {
RenderMode rendering_mode=RenderMode::Fast;
bool retain_cpu_pixel_copies() {
    const char* gpu=std::getenv("BGI_GPU");
    if(gpu && std::string_view(gpu)=="0")return true;
    const char* keep=std::getenv("BGI_KEEP_CPU_PIXELS");
    return keep && std::string_view(keep)!="0";
}

void configure_render_mode(const std::optional<std::string>& requested) {
    const char* environment=std::getenv("BGI_RENDER_MODE");
    const std::string_view mode=requested?std::string_view(*requested):
        environment?std::string_view(environment):std::string_view("fast");
    if(mode=="fast")rendering_mode=RenderMode::Fast;
    else if(mode=="exact")rendering_mode=RenderMode::Exact;
    else throw Error("render mode must be fast or exact");
}
} // namespace bgiplayer
