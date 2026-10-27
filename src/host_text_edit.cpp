#include "bgiplayer/host_text_edit.hpp"

#include <utility>
#include <array>
#include <algorithm>

namespace bgiplayer {
namespace {
#include "host_text_edit_cp932.inc"
}

std::string HostTextEdit::encode_engine_character(uint32_t cp, TextEncoding encoding) {
    if (encoding == TextEncoding::Utf8) return encode_code_point(cp, encoding);
    // The original CP932 conversion processes UTF-16 units. A non-BMP scalar
    // has no CP932 representation and produces two default characters.
    if (cp > 0xFFFF) return "??";
    const auto found = std::lower_bound(cp932_best_fit.begin(), cp932_best_fit.end(),
        cp << 16U, [](uint32_t record, uint32_t wanted) { return record < wanted; });
    if (found == cp932_best_fit.end() || (*found >> 16U) != cp) return "?";
    const uint32_t code = *found & 0xFFFFU;
    std::string output;
    if (code > 0xFF) output += static_cast<char>(code >> 8U);
    output += static_cast<char>(code & 0xFFU);
    return output;
}

uint32_t HostTextEdit::open(
    uint32_t x, uint32_t y, uint32_t width, uint32_t height,
    std::string font_name, uint32_t font_size, uint32_t text_limit,
    uint32_t focus_child, uint32_t max_width, uint32_t max_height,
    TextEncoding encoding) {
    if (width <= 7U || height <= 7U || width > max_width ||
        height > max_height) {
        return kInvalidBounds;
    }
    if (font_name.empty()) {
        return kInvalidFont;
    }
    if (font_size < 8U || font_size > 64U) {
        return kInvalidFontSize;
    }
    if (text_limit < 1U || text_limit > 256U) {
        return kInvalidTextLimit;
    }
    close();
    open_ = true;
    visible_ = true;
    x_ = x;
    y_ = y;
    width_ = width;
    height_ = height;
    font_name_.clear();
    for (const auto cp : decode_text_code_points(font_name, encoding))
        font_name_ += encode_code_point(cp, TextEncoding::Utf8);
    edit_font_size_ = font_size;
    create_color_value_ = text_limit;
    focus_child_ = focus_child != 0;
    window_text_.clear();
    for (const auto cp : decode_text_code_points(buffer_, encoding))
        window_text_ += encode_code_point(cp, TextEncoding::Utf8);
    active_alignment_ = selection_mode_;
    active_width_percent_ = font_size_;
    if (host_observer_) host_observer_(HostChange::Create);
    return kCreateOk;
}

void HostTextEdit::close() {
    const bool was_open = open_;
    open_ = false;
    visible_ = false;
    focus_child_ = false;
    if (was_open && host_observer_) host_observer_(HostChange::Destroy);
}

void HostTextEdit::set_visible(bool visible) {
    visible_ = visible;
    if (!visible) focus_child_ = false;
    if (host_observer_) host_observer_(HostChange::Visibility);
}

bool HostTextEdit::set_font_size(uint32_t size) {
    if (size < 25U || size > 200U) {
        return false;
    }
    font_size_ = size;
    return true;
}

void HostTextEdit::set_color(uint32_t value) {
    color_bgr_ =
        (((value >> 8U) & 0xFFU) | ((value & 0xFFU) << 8U)) << 8U |
        ((value >> 16U) & 0xFFU);
    if (host_observer_) host_observer_(HostChange::Style);
}

std::optional<std::string> HostTextEdit::window_text() const {
    if (!open_) {
        return std::nullopt;
    }
    return window_text_;
}

bool HostTextEdit::set_selection_mode(uint32_t mode) {
    if (mode >= 3U) {
        return false;
    }
    selection_mode_ = mode;
    return true;
}

void HostTextEdit::finish_edit(bool tab) {
    focus_child_ = false;
    if (tab || notification_value_ != 0) visible_ = false;
    if (host_observer_) host_observer_(HostChange::Visibility);
}

uint32_t HostTextEdit::utf16_length(std::string_view text) {
    uint32_t result = 0;
    for (const auto cp : decode_text_code_points(text, TextEncoding::Utf8))
        result += cp > 0xFFFF ? 2U : 1U;
    return result;
}

bool HostTextEdit::permits_replacement(std::string_view current,
    uint32_t first, uint32_t length, std::string_view insertion,
    bool direct_typing) const {
    // 0046EFF0 / 1400B80D0 count ASCII and halfwidth kana as one unit,
    // all other Unicode scalar values as two. Selection offsets are UTF-16.
    const auto weight = [](uint32_t cp) {
        return cp < 0x80 || (cp >= 0xFF61 && cp < 0xFFA0) ? 1U : 2U;
    };
    uint32_t units = 0, position = 0;
    for (const auto cp : decode_text_code_points(current, TextEncoding::Utf8)) {
        const uint32_t next = position + (cp > 0xFFFF ? 2U : 1U);
        if (next <= first || position >= static_cast<uint64_t>(first) + length)
            units += weight(cp);
        position = next;
    }
    for (const auto cp : decode_text_code_points(insertion, TextEncoding::Utf8)) {
        // Copy/backspace/paste/cut are editor actions, never literal text.
        // 0046F091..0046F0BB / 1400B815D..1400B818A reject other C0 input.
        if (direct_typing && (cp < 0x20 || (secondary_value_ != 0 &&
                                         cp >= 0x20 && cp <= 0x7F))) return false;
        units += weight(cp);
    }
    return units <= text_limit();
}

}  // namespace bgiplayer
