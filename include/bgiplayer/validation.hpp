#pragma once

#include <string_view>

namespace bgiplayer {

enum class CheckLevel { Strict, Warn };

// BGI_CHECK_LEVEL=strict|warn; defaults to warn. Invalid values are errors.
CheckLevel content_check_level();

// Content integrity checks only. Structural bounds and format-layout checks
// must not use this escape hatch. A true result has already emitted a warning.
bool allow_content_check_failure(std::string_view message);

}  // namespace bgiplayer
