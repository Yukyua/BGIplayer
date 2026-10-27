#include "bgiplayer/audio_decode_controls.hpp"
#include "bgiplayer/rcpss.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <stdexcept>
namespace bgiplayer {
void apply_audio_decode_controls(std::span<int16_t> samples, uint32_t sample_rate,
                                 uint32_t channels, const AudioDecodeControls& controls) {
    if (!channels || !sample_rate || samples.size() % channels || !std::isfinite(controls.sample_gain))
        throw std::runtime_error("invalid audio decode controls");
    if (controls.sample_gain != 1.0) {
        for (auto& sample : samples) {
            const double scaled = static_cast<double>(sample) * controls.sample_gain;
            // Original CVTTSD2SI has a signed32 indefinite result on overflow.
            const int32_t converted = scaled < -2147483648.0 || scaled >= 2147483648.0
                ? INT32_MIN : static_cast<int32_t>(scaled);
            sample = static_cast<int16_t>(std::clamp(converted, -32768, 32767));
        }
    }
    // Preserve the original uint32 arithmetic used to convert milliseconds.
    const uint32_t frames = ((controls.fade_in_ms % 1000U) * sample_rate) / 1000U +
                            sample_rate * (controls.fade_in_ms / 1000U);
    if (!frames || frames > samples.size() / channels) return;
    if (!controls.pe64_fade) {
        for (uint32_t frame = 0; frame < frames; ++frame)
            for (uint32_t c = 0; c < channels; ++c) {
                auto& sample = samples[static_cast<std::size_t>(frame) * channels + c];
                sample = static_cast<int16_t>(static_cast<int32_t>(
                    static_cast<double>(sample) * static_cast<double>(frame) / frames));
            }
        return;
    }
    // 14011E4B0 uses RCPSS followed by one Newton step. Keep every float
    // rounding boundary, including on ARM hosts with fused-multiply support.
    const float estimate = detail::rcpss_estimate(static_cast<float>(frames));
    volatile float square = estimate * estimate;
    volatile float twice = estimate + estimate;
    volatile float correction = static_cast<float>(frames) * square;
    const float inverse = twice - correction;
    for (uint32_t frame = 0; frame < frames; ++frame)
        for (uint32_t c = 0; c < channels; ++c) {
            auto& sample = samples[static_cast<std::size_t>(frame) * channels + c];
            const int32_t product = std::bit_cast<int32_t>(static_cast<uint32_t>(static_cast<int32_t>(sample)) * frame);
            volatile float scaled = static_cast<float>(product) * inverse;
            const float lower = std::floor(scaled);
            const float fraction = scaled - lower;
            constexpr float epsilon = 1.0F / 16384.0F;
            const float snapped = fraction >= 1.0F - epsilon ? std::ceil(scaled) :
                                  fraction <= epsilon ? lower : static_cast<float>(scaled);
            volatile float half = snapped + 0.5F;
            sample = static_cast<int16_t>(static_cast<int32_t>(std::floor(half)));
        }
}
} // namespace bgiplayer
