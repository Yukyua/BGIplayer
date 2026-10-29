#include "bgiplayer/audio_loading.hpp"
#include "bgiplayer/audio_decode_controls.hpp"
#include "bgiplayer/debug.hpp"
#include <chrono>
#include <cstdlib>
#include <iostream>
namespace bgiplayer {
SoundSubsystem::LoadResult load_audio_for_host(const SoundSubsystem::LoadRequest& request,
    const AudioSourceReader& reader, SdlAudioBackend& backend, bool pe64, bool trace) {
    using Kind = SoundSubsystem::LoadKind;
    if (trace) {
        std::cout << "SND_LOAD kind=" << static_cast<int>(request.kind)
            << " channel=" << request.channel << " primary=" << request.primary
            << " secondary=" << request.secondary << " tertiary=" << request.tertiary
            << " volume=" << request.volume << " pan=" << request.pan
            << " sample_gain=" << request.sample_gain << " fade_in_ms=" << request.fade_in_ms
            << " rate=" << request.playback_rate << '\n';
    }
    const auto failure = [&](uint32_t status, const char* stage) {
        if (trace) std::cout << "SND_LOAD_RESULT status=" << status
            << " source=" << request.secondary << " resource=" << request.primary
            << " stage=" << stage << '\n';
        return SoundSubsystem::LoadResult{status, 0, 0};
    };
    if (!backend.available()) return failure(SoundSubsystem::kDeviceUnavailable, "device");
    const bool profile = debug_trace(DebugTrace::GpuFrames);
    using Clock = std::chrono::steady_clock;
    const auto start = profile ? Clock::now() : Clock::time_point{};
    const bool bgm = request.kind == Kind::BgmFile || request.kind == Kind::BgmArchive || request.kind == Kind::BgmPair;
    std::optional<std::vector<uint8_t>> encoded = request.kind == Kind::RegisterEffect
        ? std::optional<std::vector<uint8_t>>(request.registration_encoded)
        : reader(request.secondary, request.primary);
    if (!encoded) return failure(0x0CU, "read");
    const auto read_end = profile ? Clock::now() : Clock::time_point{};
    if (bgm && backend.reload_cached_bgm(request.channel, *encoded, request.playback_rate)) {
        if (profile) std::cerr << "AUDIO_LOAD_COST cache_hit=1 encoded_bytes=" << encoded->size()
            << " read_ns=" << std::chrono::duration_cast<std::chrono::nanoseconds>(read_end-start).count()
            << " decode_ns=0 backend_ns=" << std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-read_end).count() << '\n';
        if (trace) std::cout << "SND_LOAD_RESULT status=0 source=" << request.secondary
            << " resource=" << request.primary << " encoded_bytes=" << encoded->size()
            << " cached_bgm=1\n";
        return {0,0,0};
    }
    const char* streaming = std::getenv("BGI_AUDIO_STREAMING");
    const bool stream_enabled = !streaming || std::string_view(streaming) != "0";
    if (bgm && stream_enabled) {
        // Host output preparation only. The original operand/order/channel state
        // remains synchronous; a validated PCM prefix is ready before success.
        auto progressive = ProgressiveAudio::open(*encoded);
        if (progressive) {
            const auto decode_end = profile ? Clock::now() : Clock::time_point{};
            if (!backend.load_progressive_bgm(request.channel, std::move(progressive),
                    request.playback_rate, *encoded)) return failure(0x0EU, "progressive backend");
            if (profile) std::cerr << "AUDIO_LOAD_COST cache_hit=0 progressive=1 encoded_bytes=" << encoded->size()
                << " read_ns=" << std::chrono::duration_cast<std::chrono::nanoseconds>(read_end-start).count()
                << " decode_ns=" << std::chrono::duration_cast<std::chrono::nanoseconds>(decode_end-read_end).count()
                << " backend_ns=" << std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-decode_end).count() << '\n';
            if (trace) std::cout << "SND_LOAD_RESULT status=0 source=" << request.secondary
                << " resource=" << request.primary << " encoded_bytes=" << encoded->size()
                << " progressive=1\n";
            return {0,0,0};
        }
    }
    AudioDecodeControls controls{request.sample_gain,request.fade_in_ms,pe64};
    auto decoded = decode_audio(*encoded,nullptr,bgm?nullptr:&controls);
    if (!decoded) return failure(0x0EU, "decode");
    const auto decode_end = profile ? Clock::now() : Clock::time_point{};
    const auto sample_count=decoded->samples.size();
    if (!backend.load(bgm,request.channel,std::move(*decoded),request.playback_rate,
            bgm ? std::span<const uint8_t>(*encoded) : std::span<const uint8_t>{})) return failure(0x0EU, "backend");
    if (profile) std::cerr << "AUDIO_LOAD_COST cache_hit=0 encoded_bytes=" << encoded->size()
        << " read_ns=" << std::chrono::duration_cast<std::chrono::nanoseconds>(read_end-start).count()
        << " decode_ns=" << std::chrono::duration_cast<std::chrono::nanoseconds>(decode_end-read_end).count()
        << " backend_ns=" << std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-decode_end).count() << '\n';
    if (trace) std::cout << "SND_LOAD_RESULT status=0 source=" << request.secondary
        << " resource=" << request.primary << " encoded_bytes=" << encoded->size()
        << " decoded_samples=" << sample_count << '\n';
    return {0,0,0};
}
} // namespace bgiplayer
