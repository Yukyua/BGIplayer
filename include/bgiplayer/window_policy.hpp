#pragma once
#include <cstdint>
#ifdef __APPLE__
#include <TargetConditionals.h>
#endif
namespace bgiplayer {
enum class HostWindowPlatform { Desktop, IOS, Android };
constexpr HostWindowPlatform host_window_platform() {
#if defined(__ANDROID__)
    return HostWindowPlatform::Android;
#elif defined(__APPLE__) && TARGET_OS_IPHONE
    return HostWindowPlatform::IOS;
#else
    return HostWindowPlatform::Desktop;
#endif
}
constexpr bool host_window_changes_supported(HostWindowPlatform platform=host_window_platform()) {
    return platform==HostWindowPlatform::Desktop;
}
// Initial script configuration establishes the logical game canvas before the
// native view exists. Once attached, mobile mode identity is immutable.
constexpr bool host_window_mode_change_allowed(HostWindowPlatform platform,
    bool attached, uint32_t fullscreen, uint32_t flag, uint32_t index,
    uint32_t current_fullscreen, uint32_t current_flag, uint32_t current_index) {
    return host_window_changes_supported(platform) || !attached ||
        (fullscreen==current_fullscreen && flag==current_flag && index==current_index);
}
constexpr bool host_client_resize_allowed(HostWindowPlatform platform, bool attached) {
    return host_window_changes_supported(platform) || !attached;
}
}
