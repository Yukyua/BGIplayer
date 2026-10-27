#pragma once

#include <filesystem>
#include <string_view>

namespace bgiplayer {
class VM;
class SdlAudioBackend;
// Debug checkpoints are versioned implementation state, separate from game saves.
void save_vm_snapshot(VM& vm, const std::filesystem::path& path,
                      std::string_view game_identity,
                      SdlAudioBackend* audio = nullptr);
void load_vm_snapshot(VM& vm, const std::filesystem::path& path,
                      std::string_view game_identity,
                      SdlAudioBackend* audio = nullptr);
}
