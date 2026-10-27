#pragma once

#include <array>
#include <cstdint>

namespace bgiplayer {

class VM;
struct Thread;

enum class NativeSignal {
    Continue,
    Yield,
    ResumeCurrent,
    Switch,
    Pause,
    NextThread,
    EndPass,
};

class NativeRegistry {
public:
    using Handler = NativeSignal (*)(VM&, Thread&);

    NativeRegistry();

    NativeSignal invoke(uint8_t family, uint8_t sub, VM& vm, Thread& thread) const;
    [[nodiscard]] bool implemented(uint8_t family, uint8_t sub) const;

private:
    static constexpr std::size_t kFamilies = 11;
    static constexpr std::size_t kSlots = 256;
    std::array<std::array<Handler, kSlots>, kFamilies> handlers_{};
};

std::size_t native_family_index(uint8_t family);

}  // namespace bgiplayer
