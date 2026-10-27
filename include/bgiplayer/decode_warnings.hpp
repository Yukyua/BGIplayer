#pragma once

#include <cstdint>
#include <span>
#include <string_view>

namespace bgiplayer {
// Final decode failures only. Format probes and successful fallback attempts
// must not call this. Diagnostic output does not change failure semantics.
void warn_decode_failure(std::string_view decoder, std::string_view reason,
                         std::span<const uint8_t> encoded,
                         std::string_view resource = {});
} // namespace bgiplayer
