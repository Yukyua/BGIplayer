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
public:
    SdlAudioBackend();
    ~SdlAudioBackend();

    SdlAudioBackend(const SdlAudioBackend&) = delete;
    SdlAudioBackend& operator=(const SdlAudioBackend&) = delete;

    bool initialize();
    void shutdown();
    [[nodiscard]] bool available() const;

    bool load(bool bgm, uint32_t channel, DecodedAudio audio,
              double playback_rate);
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
        double position = 0.0;
        uint64_t observed_effect_generation = 0;
        double playback_rate = 1.0;
        double left_gain = 1.0;
        double right_gain = 1.0;
        bool active = false;
        bool bgm = false;
        bool paused = false;
    };

    void mix(int16_t* stream, int length);
    static void audio_callback(void* userdata, uint8_t* stream,
                               int length);
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
};

}  // namespace bgiplayer
