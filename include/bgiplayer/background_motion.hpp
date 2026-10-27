#pragma once
#include <cstdint>
#include <optional>
#include "bgiplayer/background_transform.hpp"
namespace bgiplayer {
struct BackgroundTranslation { int32_t x; int32_t y; };
// 004234A0 resolves origin + ((signed delta * 0041F6A0(time, selector)) >>16).
std::optional<BackgroundTranslation> evaluate_background_translation(
    int32_t x, int32_t y, int32_t delta_x, int32_t delta_y,
    int32_t phase_q24, uint32_t selector);
// 004234A0's three 0041F6A0 evaluations: position selector +184, angle
// selector +188 and scale selector +18C. The angle uses the same Q16 degree
// units as the slot. The scale uses the original reciprocal interpolation,
// which is not a linear fixed-point blend.
std::optional<BackgroundTransform> evaluate_background_transform(
    int32_t origin_x, int32_t origin_y, int32_t velocity_x,
    int32_t velocity_y, int32_t angle, int32_t angle_velocity,
    int32_t scale_x, int32_t scale_y, int32_t scale_velocity_x,
    int32_t scale_velocity_y, int32_t phase_q24,
    uint32_t position_selector, uint32_t angle_selector,
    uint32_t scale_selector);
}
