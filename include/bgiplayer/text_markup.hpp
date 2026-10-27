#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace bgiplayer {

// A nonempty <...> marker before the terminating NUL. An unmatched '<'
// (and the empty pair '<>') stays in the original literal-glyph path.
bool has_complete_text_markup(std::u16string_view text);

// User-selected presentation fallback, not original parser semantics. Strict
// mode refuses it. Warn mode removes complete <...> markers, preserving their
// body, unmatched '<' characters and all input control code units unchanged.
// Call only after determining that the original markup branch is unsupported.
std::optional<std::u16string> fallback_unsupported_text_markup(std::u16string_view text);

} // namespace bgiplayer
