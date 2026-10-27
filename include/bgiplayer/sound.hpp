#pragma once

#include "bgiplayer/common.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <utility>
#include <string>
#include <string_view>
#include <vector>

namespace bgiplayer {

// Engine-side state for the WaveMaster sound-device layer used by Snd0.
// Host decoding/mixing is deliberately kept behind this subsystem so the
// native semantic layer remains usable with the null backend.
class SoundSubsystem {
    friend struct SnapshotCodec;
public:
    static constexpr uint32_t kOk = 0;
    static constexpr uint32_t kMissingObject = 0x13;
    static constexpr uint32_t kDeviceUnavailable = 0x14;
    static constexpr uint32_t kChannelOutOfRange = 0x15;

    struct FadeState {
        bool active = false;
        uint32_t start_ms = 0;
        uint32_t deadline_ms = 0;
        uint32_t from = 0;
        uint32_t to = 0;
        uint32_t current = 0;
    };

    struct ChannelState {
        bool opened = false;
        bool control_value = false;
        // Restart identity for the batched host boundary; survives snapshots.
        uint64_t effect_play_generation = 0;
        uint32_t position = 0;
        uint32_t device_state = 0;
        // Original channel +0x0C (PE32), +0x18 (PE64): Snd0:08/09.
        uint32_t primary_volume = 0x80;
        uint32_t secondary_volume = 0x80;
        uint32_t volume_level = 0;
        uint32_t full_level = 0;
        uint32_t pan = 0x80;
        uint32_t registration_value = 0;
        std::array<uint8_t, 0x40> registration_block{};
        FadeState volume_fade;
        FadeState full_fade;
        FadeState silence_fade;
    };

    struct CdAudioQuery {
        bool command_succeeded = false;
        uint32_t value = 0;
    };

    struct CdAudioHooks {
        std::function<bool()> open;
        std::function<bool()> close;
        std::function<CdAudioQuery(uint32_t)> status;
        std::function<bool(uint32_t, uint32_t)> play;
        std::function<bool()> stop;
    };

    struct CdModeResult {
        uint32_t bp_result = 0;
        bool wrote_output = false;
        uint32_t mode = 0;
    };

    enum class LoadKind {
        BgmFile,
        BgmArchive,
        BgmPair,
        Effect,
        EffectScaled,
        EffectDoubleRate,
        EffectCustomRate,
        RegisterEffect,
    };

    struct LoadRequest {
        LoadKind kind = LoadKind::BgmFile;
        uint32_t channel = 0;
        std::string primary;
        std::string secondary;
        std::string tertiary;
        uint32_t volume = 0x80;
        uint32_t pan = 0x40;
        uint32_t registration_value = 0;
        std::array<uint8_t, 0x40> registration_block{};
        double registration_volume = 0.0;
        double registration_rate = 0.0;
        // Loader gain scales decoded PCM; this is not speaker pan.
        double sample_gain = 1.0;
        // Original static cache's precomputed fade-in duration.
        uint32_t fade_in_ms = 0;
        std::vector<uint8_t> registration_encoded;
        double playback_rate = 1.0;
        // Snd0:0x12 raw operand, forwarded to 0x004C30B0/0x004CAA40.
        uint32_t bgm_pair_selector = 0;
    };

    struct LoadResult {
        uint32_t status = 0;
        uint32_t output1 = 0;
        uint32_t output2 = 0;
    };

    struct LoadPoll {
        int status = 0;
        std::vector<uint32_t> outputs;
    };

    using LoadProvider =
        std::function<std::optional<LoadResult>(const LoadRequest&)>;

    void set_device_ready(bool ready) { device_ready_ = ready; }
    void set_timer_enabled(bool enabled, uint32_t now_ms);
    void set_cd_audio_hooks(CdAudioHooks hooks);
    void set_wave_player(
        std::function<bool(std::string_view)> callback);
    void set_load_provider(LoadProvider provider);
    using BgmStatusProvider = std::function<std::pair<uint32_t, uint32_t>(uint32_t)>;
    void set_bgm_status_provider(BgmStatusProvider provider) {
        bgm_status_provider_ = std::move(provider);
    }
    [[nodiscard]] bool device_ready() const { return device_ready_; }
    void update(uint32_t now_ms);

    [[nodiscard]] uint32_t check_bgm(uint32_t channel) const;
    [[nodiscard]] uint32_t check_effect(uint32_t channel) const;
    [[nodiscard]] static uint32_t stream_volume(
        uint32_t primary_volume, uint32_t secondary_volume,
        uint32_t volume, uint32_t full_volume);
    [[nodiscard]] static int32_t pan_exponent(int32_t pan);
    [[nodiscard]] static double volume_gain(uint32_t level);
    [[nodiscard]] static double master_gain(
        int32_t hundredths_db);
    [[nodiscard]] static std::pair<double, double> pan_gains(
        int32_t pan);

    void open_bgm(uint32_t channel, uint32_t state = 0,
                  uint32_t position = 0);
    void open_effect(uint32_t channel, uint32_t position = 0);
    void close_bgm(uint32_t channel);
    void close_effect(uint32_t channel);

    uint32_t control_bgm(uint32_t channel, uint32_t action);
    [[nodiscard]] uint32_t bgm_state(uint32_t channel) const;
    [[nodiscard]] std::optional<uint32_t> bgm_position(
        uint32_t channel) const;

    uint32_t fade_bgm_volume(uint32_t channel, uint32_t volume,
                             uint32_t duration_ms);
    uint32_t fade_bgm_full(uint32_t channel, uint32_t duration_ms);
    uint32_t fade_bgm_silence(uint32_t channel, uint32_t duration_ms);
    uint32_t fade_effect_silence(uint32_t channel, uint32_t duration_ms);
    uint32_t set_bgm_pan(uint32_t channel, uint32_t pan);
    uint32_t set_bgm_primary_volume(uint32_t channel, uint32_t volume);
    uint32_t set_effect_primary_volume(uint32_t channel, uint32_t volume);
    uint32_t set_bgm_subtle_volume(uint32_t channel, uint32_t volume);
    uint32_t set_effect_subtle_volume(uint32_t channel, uint32_t volume);

    uint32_t release_effect(uint32_t channel);
    uint32_t stop_effect(uint32_t channel);
    [[nodiscard]] uint32_t effect_position(uint32_t channel) const;
    void start_effect_playback(uint32_t channel, uint32_t volume,
                               uint32_t pan);
    void update_bgm_position(uint32_t channel, uint32_t position);
    void update_effect_position(uint32_t channel, uint32_t position);

    void open_cd_audio();
    void close_cd_audio();
    bool play_cd_track(uint32_t track, uint32_t option);
    void stop_cd_audio();
    [[nodiscard]] CdModeResult query_cd_mode() const;
    bool play_wave(std::string_view path);
    [[nodiscard]] bool cd_audio_open() const { return cd_audio_open_; }
    [[nodiscard]] uint32_t last_cd_track() const {
        return last_cd_track_;
    }
    uint32_t enqueue_load(LoadRequest request);
    LoadPoll poll_load(uint32_t task);
    LoadResult load_now(const LoadRequest& request);
    [[nodiscard]] const LoadRequest& load_request(uint32_t task) const;

    [[nodiscard]] const ChannelState& bgm(uint32_t channel) const;
    [[nodiscard]] const ChannelState& effect(uint32_t channel) const;

private:
    static uint32_t current_time_ms();
    static FadeState make_fade(uint32_t from, uint32_t to,
                               uint32_t duration_ms);
    static void update_fade(FadeState& fade, uint32_t now_ms);
    void apply_load(const LoadRequest& request,
                    const LoadResult& result);

    bool device_ready_ = false;
    bool timer_enabled_ = false;
    uint32_t next_timer_tick_ms_ = 0;
    bool cd_audio_open_ = false;
    uint32_t last_cd_track_ = 0;
    CdAudioHooks cd_audio_hooks_;
    std::function<bool(std::string_view)> wave_player_;
    LoadProvider load_provider_;
    BgmStatusProvider bgm_status_provider_;
    struct LoadTask {
        LoadRequest request;
        bool started = false;
    };
    uint32_t next_load_task_ = 1;
    std::map<uint32_t, LoadTask> load_tasks_;
    std::array<ChannelState, 16> bgm_channels_{};
    std::array<ChannelState, 128> effect_channels_{};
};

}  // namespace bgiplayer
