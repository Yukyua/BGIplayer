#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace bgiplayer {

struct AudioDecodeControls;

struct DecodedAudio {
    static constexpr uint32_t kSampleRate = 48000;
    static constexpr uint32_t kChannels = 2;
    std::vector<int16_t> samples;
};

std::optional<DecodedAudio> decode_audio(
    std::span<const uint8_t> encoded, std::string* error = nullptr,
    const AudioDecodeControls* controls = nullptr);

}  // namespace bgiplayer
