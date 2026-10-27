#pragma once
#include <cstdint>
#include <optional>
namespace bgiplayer {
// Original0041F6A0 / PE64 140057070: ECX is a signed Q24 phase (often object+ C0),
// EDX is the selector. The returned factor is signed Q16.
std::optional<int32_t> evaluate_display_easing(int32_t phase_q24, uint32_t selector);
}
