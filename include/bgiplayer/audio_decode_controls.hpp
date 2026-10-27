#pragma once
#include <cstdint>
#include <span>
namespace bgiplayer {
struct AudioDecodeControls {
    double sample_gain = 1.0;
    uint32_t fade_in_ms = 0;
    bool pe64_fade = false;
};
// Engine PCM transformations, before the host output-rate/channel conversion.
void apply_audio_decode_controls(std::span<int16_t> samples, uint32_t sample_rate,
                                 uint32_t channels, const AudioDecodeControls& controls);
} // namespace bgiplayer
