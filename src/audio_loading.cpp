#include "bgiplayer/audio_loading.hpp"
#include "bgiplayer/audio_decode_controls.hpp"
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
    const bool bgm = request.kind == Kind::BgmFile || request.kind == Kind::BgmArchive || request.kind == Kind::BgmPair;
    std::optional<std::vector<uint8_t>> encoded = request.kind == Kind::RegisterEffect
        ? std::optional<std::vector<uint8_t>>(request.registration_encoded)
        : reader(request.secondary, request.primary);
    if (!encoded) return failure(0x0CU, "read");
    AudioDecodeControls controls{request.sample_gain,request.fade_in_ms,pe64};
    auto decoded = decode_audio(*encoded,nullptr,bgm?nullptr:&controls);
    if (!decoded) return failure(0x0EU, "decode");
    const auto sample_count=decoded->samples.size();
    if (!backend.load(bgm,request.channel,std::move(*decoded),request.playback_rate)) return failure(0x0EU, "backend");
    if (trace) std::cout << "SND_LOAD_RESULT status=0 source=" << request.secondary
        << " resource=" << request.primary << " encoded_bytes=" << encoded->size()
        << " decoded_samples=" << sample_count << '\n';
    return {0,0,0};
}
} // namespace bgiplayer
