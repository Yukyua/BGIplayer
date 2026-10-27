#include "bgiplayer/background_motion.hpp"
#include "bgiplayer/display_easing.hpp"
#include <cmath>
#include <limits>
namespace bgiplayer {
namespace {
int32_t original_truncate(double value) {
    // CVTTSD2SI leaves 0x80000000 for infinities/out-of-range values.
    if (!std::isfinite(value) || value <= -2147483649.0 ||
        value >= 2147483648.0)
        return std::numeric_limits<int32_t>::min();
    return static_cast<int32_t>(value);
}
// 004234A0: 65536 / ((65536/(base+delta) - 65536/base) * factor/65536 +
// 65536/base). The original evaluates this in double and truncates.
int32_t interpolate_scale(int32_t base, int32_t delta, int32_t factor) {
    constexpr double k = 65536.0;
    const double start = k / static_cast<double>(base);
    const double end = k / static_cast<double>(base + delta);
    const double value =
        k / ((end - start) * static_cast<double>(factor) * (1.0 / 65536.0) +
             start);
    return original_truncate(value);
}
}  // namespace

std::optional<BackgroundTranslation> evaluate_background_translation(
    int32_t x, int32_t y, int32_t delta_x, int32_t delta_y,
    int32_t phase_q24, uint32_t selector) {
    const auto phase = evaluate_display_easing(phase_q24, selector);
    if (!phase) return std::nullopt;
    const auto resolve = [phase](int32_t origin, int32_t delta) {
        const int64_t product = static_cast<int64_t>(delta) * *phase;
        return static_cast<int32_t>(static_cast<uint32_t>(origin) +
                                    static_cast<uint32_t>(product >> 16));
    };
    return BackgroundTranslation{resolve(x, delta_x), resolve(y, delta_y)};
}

std::optional<BackgroundTransform> evaluate_background_transform(
    int32_t origin_x, int32_t origin_y, int32_t velocity_x,
    int32_t velocity_y, int32_t angle, int32_t angle_velocity,
    int32_t scale_x, int32_t scale_y, int32_t scale_velocity_x,
    int32_t scale_velocity_y, int32_t phase_q24,
    uint32_t position_selector, uint32_t angle_selector,
    uint32_t scale_selector) {
    const auto position_phase =
        evaluate_display_easing(phase_q24, position_selector);
    const auto angle_phase =
        evaluate_display_easing(phase_q24, angle_selector);
    const auto scale_phase =
        evaluate_display_easing(phase_q24, scale_selector);
    if (!position_phase || !angle_phase || !scale_phase) return std::nullopt;
    // The original computes the angle and both scales in the same 32-bit
    // wrapping arithmetic as the translation.
    const auto resolve = [](int32_t base, int32_t delta, int32_t factor) {
        const int64_t product = static_cast<int64_t>(delta) * factor;
        return static_cast<int32_t>(static_cast<uint32_t>(base) +
                                    static_cast<uint32_t>(product >> 16));
    };
    if (scale_x <= 0 || scale_y <= 0 || scale_x + scale_velocity_x <= 0 ||
        scale_y + scale_velocity_y <= 0)
        return std::nullopt;
    BackgroundTransform result;
    result.origin_x = resolve(origin_x, velocity_x, *position_phase);
    result.origin_y = resolve(origin_y, velocity_y, *position_phase);
    result.angle = resolve(angle, angle_velocity, *angle_phase);
    result.scale_x = interpolate_scale(scale_x, scale_velocity_x, *scale_phase);
    result.scale_y = interpolate_scale(scale_y, scale_velocity_y, *scale_phase);
    return result;
}
}  // namespace bgiplayer
