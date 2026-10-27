#include "bgiplayer/window_sizing.hpp"
#include "bgiplayer/rcpss.hpp"
#include <cmath>
#include "bgiplayer/vm.hpp"
#if defined(__clang__)
#pragma clang fp contract(off)
#endif
namespace bgiplayer {
std::optional<std::pair<uint32_t,uint32_t>> checked_window_sizing_limit(
    uint32_t canvas_width,uint32_t canvas_height,int32_t width,int32_t height) {
    if(width<=0 || height<=0) {width=static_cast<int32_t>(canvas_width);height=static_cast<int32_t>(canvas_height);}
    // Preserve the original SSE single precision reciprocal estimate and one
    // Newton refinement, with a rounded float result at every instruction.
    const float canvas_h=static_cast<float>(static_cast<int32_t>(canvas_height));
    const float limit_h=static_cast<float>(height);
    const float c=detail::rcpss_estimate(canvas_h),l=detail::rcpss_estimate(limit_h);
    const float c_square=c*c,l_square=l*l;
    const float c_twice=c+c,l_twice=l+l;
    const float c_error=canvas_h*c_square,l_error=limit_h*l_square;
    const float c_refined=c_twice-c_error,l_refined=l_twice-l_error;
    const float original_ratio=c_refined*static_cast<float>(static_cast<int32_t>(canvas_width));
    const float limit_ratio=l_refined*static_cast<float>(width);
    const float difference=original_ratio-limit_ratio;
    if(std::abs(static_cast<double>(difference))>0.001)return std::nullopt;
    return std::pair{static_cast<uint32_t>(width),static_cast<uint32_t>(height)};
}
bool VM::set_window_sizing_limit(int32_t width,int32_t height) {
    const auto limit=checked_window_sizing_limit(window_width(window_mode_index_),
        window_height(window_mode_index_),width,height);
    if(!limit)return false;
    sizing_limit_width_=limit->first;sizing_limit_height_=limit->second;
    if(window_sizing_limit_handler_)
        window_sizing_limit_handler_(sizing_limit_width_,sizing_limit_height_);
    return true;
}
}
