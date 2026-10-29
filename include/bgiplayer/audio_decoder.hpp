#pragma once

#include <cstdint>
#include <functional>
#include <memory>
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

using AudioPcmSink = std::function<bool(std::span<const int16_t>)>;
using AudioPcmProducer = std::function<bool(const AudioPcmSink&, std::string*)>;

// Host-only incremental PCM result. Publication is immutable and acquire/release;
// sample() never waits, allocates or runs a decoder on the device callback.
class ProgressiveAudio {
public:
    static std::shared_ptr<ProgressiveAudio> open(std::vector<uint8_t> encoded,
                                                std::string* error = nullptr);
    // A producer must stop when its sink returns false. Primarily useful for
    // alternative host decoders and deterministic callback/lifecycle checks.
    static std::shared_ptr<ProgressiveAudio> start(AudioPcmProducer producer,
                                                 std::string* error = nullptr);
    ~ProgressiveAudio();
    ProgressiveAudio(const ProgressiveAudio&) = delete;
    ProgressiveAudio& operator=(const ProgressiveAudio&) = delete;
    [[nodiscard]] uint64_t available_frames() const noexcept;
    [[nodiscard]] bool complete() const noexcept;
    [[nodiscard]] bool failed() const noexcept;
    [[nodiscard]] uint64_t retained_bytes() const noexcept;
    // Error becomes immutable at complete(); only called by the owner thread.
    [[nodiscard]] std::string error_text() const;
    [[nodiscard]] bool sample(uint64_t frame, int16_t& left, int16_t& right) const noexcept;
    // Debug snapshot boundary only; never called by the device callback.
    std::shared_ptr<const DecodedAudio> completed_clip();
private:
    struct State;
    explicit ProgressiveAudio(std::unique_ptr<State> state);
    static std::shared_ptr<ProgressiveAudio> start_impl(AudioPcmProducer producer,
        size_t encoded_capacity, std::string* error);
    std::unique_ptr<State> state_;
};

// Sink false cancels decoding; successful completion uses exactly the same
// FFmpeg state and output conversion as decode_audio (no per-chunk seek).
bool decode_audio_incremental(std::span<const uint8_t> encoded,
                              const AudioPcmSink& sink,
                              std::string* error = nullptr);

}  // namespace bgiplayer
