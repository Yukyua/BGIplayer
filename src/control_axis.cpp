#include "bgiplayer/control_axis.hpp"
#include <algorithm>
namespace bgiplayer {
ControlAxisGeometry control_axis_geometry(uint32_t first,uint32_t last,
                                         int32_t target_end,int32_t divisions) {
    const int32_t delta = static_cast<int32_t>(last-first-static_cast<uint32_t>(target_end));
    const int32_t fixed = static_cast<int32_t>(static_cast<uint32_t>(delta)<<16U);
    const int32_t step = std::max(divisions > 2 ? fixed/(divisions-1) : fixed,1);
    return {divisions <= 0 ? delta : divisions == 1 ? 0 : fixed/step,step};
}
int32_t control_axis_pixel_offset(int32_t value,int32_t divisions,
                                  const ControlAxisGeometry& geometry) {
    if (divisions <= 0) return value;
    return static_cast<int32_t>(static_cast<uint32_t>(value)*
        (static_cast<uint32_t>(geometry.step_q16)+1U))>>16;
}
}
