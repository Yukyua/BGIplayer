#include "bgiplayer/text_markup.hpp"
#include "bgiplayer/validation.hpp"

#include <atomic>
#include <iostream>

namespace bgiplayer {

bool has_complete_text_markup(std::u16string_view text) {
    text = text.substr(0, text.find(u'\0'));
    for (auto marker = text.find('<'); marker != text.npos;
         marker = text.find('<', marker + 1)) {
        const auto end = text.find('>', marker + 1);
        if (end != text.npos && end > marker + 1) return true;
    }
    return false;
}

std::optional<std::u16string> fallback_unsupported_text_markup(std::u16string_view text) {
    if (content_check_level() == CheckLevel::Strict) return std::nullopt;
    static std::atomic_flag reported = ATOMIC_FLAG_INIT;
    if (!reported.test_and_set())
        std::cerr << "warning: unsupported text markup; displaying body without complete tags "
                     "(BGI_CHECK_LEVEL=warn)\n";
    std::u16string body;
    body.reserve(text.size());
    for (std::size_t i = 0; i < text.size();) {
        if (text[i] == '<') {
            const auto end = text.find('>', i + 1);
            if (end != text.npos && end > i + 1) {
                // A formatting fallback must not erase engine controls even
                // when an unsupported marker happens to contain one.
                for (auto at = i + 1; at < end; ++at)
                    if (text[at] < 0x20) body.push_back(text[at]);
                i = end + 1;
                continue;
            }
        }
        body.push_back(text[i++]);
    }
    return body;
}

} // namespace bgiplayer
