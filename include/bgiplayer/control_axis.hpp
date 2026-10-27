#pragma once
#include <cstdint>
namespace bgiplayer {
struct ControlAxisGeometry { int32_t limit = 0; int32_t step_q16 = 0; };
// Original CDspObjKnob 00426870/004268D0 and 00426930.
ControlAxisGeometry control_axis_geometry(uint32_t first, uint32_t last,
                                         int32_t target_end, int32_t divisions);
int32_t control_axis_pixel_offset(int32_t value, int32_t divisions,
                                  const ControlAxisGeometry& geometry);
}
