#include "bgiplayer/validation.hpp"
#include "bgiplayer/common.hpp"

#include <cstdlib>
#include <iostream>

namespace bgiplayer {
CheckLevel content_check_level() {
    const char* configured = std::getenv("BGI_CHECK_LEVEL");
    if (configured != nullptr && std::string_view(configured) == "strict")
        return CheckLevel::Strict;
    if (configured == nullptr || std::string_view(configured) == "warn")
        return CheckLevel::Warn;
    throw Error("BGI_CHECK_LEVEL must be strict or warn");
}

bool allow_content_check_failure(std::string_view message) {
    if (content_check_level() == CheckLevel::Strict) return false;
    std::cerr << "warning: " << message << "; continuing (BGI_CHECK_LEVEL=warn)\n";
    return true;
}
}  // namespace bgiplayer
