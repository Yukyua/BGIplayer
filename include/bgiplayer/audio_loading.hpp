#pragma once
#include "bgiplayer/audio_backend.hpp"
#include "bgiplayer/sound.hpp"
#include <functional>
namespace bgiplayer {
using AudioSourceReader = std::function<std::optional<std::vector<uint8_t>>(std::string_view, std::string_view)>;
SoundSubsystem::LoadResult load_audio_for_host(const SoundSubsystem::LoadRequest& request,
    const AudioSourceReader& reader, SdlAudioBackend& backend, bool pe64, bool trace);
} // namespace bgiplayer
