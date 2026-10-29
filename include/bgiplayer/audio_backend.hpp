#pragma once

#include "bgiplayer/audio_decoder.hpp"
#include "bgiplayer/sound.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <map>
#include <mutex>

namespace bgiplayer {

class SdlAudioBackend {
    friend struct SnapshotCodec;
    friend struct AudioBackendTestAccess;
public:
    SdlAudioBackend();
    ~SdlAudioBackend();

    SdlAudioBackend(const SdlAudioBackend&) = delete;
    SdlAudioBackend& operator=(const SdlAudioBackend&) = delete;

    bool initialize();
    void shutdown();
    [[nodiscard]] bool available() const;

    bool load(bool bgm, uint32_t channel, DecodedAudio audio,
              double playback_rate, std::span<const uint8_t> bgm_encoded = {});
    bool reload_cached_bgm(uint32_t channel, std::span<const uint8_t> encoded,
                           double playback_rate);
    bool load_progressive_bgm(uint32_t channel, std::shared_ptr<ProgressiveAudio> audio,
                              double playback_rate, std::span<const uint8_t> encoded);
    // Explicit debug snapshot boundary. Drains workers before the audio mutex.
    bool prepare_snapshot();
    void stop(bool bgm, uint32_t channel);
    bool trigger_effect(uint32_t channel);
    bool play_oneshot(DecodedAudio audio);
    bool load_movie(uint32_t key, DecodedAudio audio);
    void sync_movie(uint32_t key, uint32_t position_ms,
                    bool playing, uint32_t volume, bool seek = false,
                    bool linear_gain = false);
    void stop_movie(uint32_t key);
    [[nodiscard]] uint32_t movie_position_ms(uint32_t key) const;
    void sync(const SoundSubsystem& sound, int32_t master_level);
    [[nodiscard]] uint32_t playback_state(bool bgm, uint32_t channel) const;
    [[nodiscard]] uint32_t position_ms(
        bool bgm, uint32_t channel) const;
    static uint64_t align_resume_frame(uint64_t position) {
        return position - position % 5U;
    }

private:
    struct Track {
        std::shared_ptr<const DecodedAudio> clip;
        std::shared_ptr<ProgressiveAudio> progressive;
        bool progressive_failure_reported = false;
        double position = 0.0;
        uint64_t observed_effect_generation = 0;
        double playback_rate = 1.0;
        double left_gain = 1.0;
        double right_gain = 1.0;
        bool active = false;
        bool bgm = false;
        bool paused = false;
    };

    void retire_progressive_tracks();
    void mix(int16_t* stream, int length);
    static void audio_callback(void* userdata, uint8_t* stream,
                               int length);
    bool load_shared(bool bgm, uint32_t channel,
                     std::shared_ptr<const DecodedAudio> clip,
                     double playback_rate);
    Track& track(bool bgm, uint32_t channel);
    const Track& track(bool bgm, uint32_t channel) const;

    mutable std::mutex mutex_;
    std::array<Track, 16> bgm_tracks_{};
    std::array<Track, 128> effect_tracks_{};
    std::array<Track, 8> one_shot_tracks_{};
    std::map<uint32_t, Track> movie_tracks_;
    uint32_t next_one_shot_ = 0;
    uint32_t device_id_ = 0;
    bool initialized_ = false;
    // One immutable host-side decode result; never part of VM save state.
    std::vector<uint8_t> cached_bgm_encoded_;
    std::shared_ptr<const DecodedAudio> cached_bgm_clip_;
    std::shared_ptr<ProgressiveAudio> cached_bgm_progressive_;
    uint32_t cached_bgm_sample_rate_ = 0;
    uint32_t cached_bgm_channels_ = 0;
};

}  // namespace bgiplayer
