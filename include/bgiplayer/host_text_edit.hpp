#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <functional>
#include <string_view>
#include "bgiplayer/text.hpp"

namespace bgiplayer {

class HostTextEdit {
    friend struct SnapshotCodec;
public:
    static constexpr uint32_t kCreateOk = 0;
    static constexpr uint32_t kInvalidBounds = 1;
    static constexpr uint32_t kInvalidFont = 2;
    static constexpr uint32_t kInvalidFontSize = 3;
    static constexpr uint32_t kInvalidColor = 4;
    static constexpr uint32_t kInvalidTextLimit = 4;

    uint32_t open(uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                  std::string font_name, uint32_t font_size, uint32_t text_limit,
                  uint32_t focus_child, uint32_t max_width,
                  uint32_t max_height, TextEncoding encoding = TextEncoding::Utf8);
    void close();
    [[nodiscard]] bool is_open() const { return open_; }
    [[nodiscard]] bool visible() const { return visible_; }
    void set_visible(bool visible);

    [[nodiscard]] bool set_font_size(uint32_t size);
    [[nodiscard]] uint32_t font_size() const { return font_size_; }
    [[nodiscard]] uint32_t edit_font_size() const { return edit_font_size_; }
    [[nodiscard]] uint32_t create_color_value() const {
        return create_color_value_;
    }
    [[nodiscard]] bool focus_child() const { return focus_child_; }
    void set_color(uint32_t value);
    [[nodiscard]] uint32_t color() const { return color_bgr_; }
    void set_buffer(std::string text) { buffer_ = std::move(text); }
    [[nodiscard]] const std::string& buffer() const { return buffer_; }
    [[nodiscard]] std::optional<std::string> window_text() const;
    void set_notification_value(uint32_t value) { notification_value_ = value; }
    [[nodiscard]] uint32_t notification_value() const {
        return notification_value_;
    }
    void set_secondary_value(uint32_t value) { secondary_value_ = value; }
    [[nodiscard]] uint32_t secondary_value() const { return secondary_value_; }
    [[nodiscard]] bool set_selection_mode(uint32_t mode);
    [[nodiscard]] uint32_t selection_mode() const { return selection_mode_; }
    [[nodiscard]] uint32_t active_alignment() const { return active_alignment_; }
    [[nodiscard]] uint32_t active_width_percent() const { return active_width_percent_; }
    [[nodiscard]] uint32_t x() const { return x_; }
    [[nodiscard]] uint32_t y() const { return y_; }
    [[nodiscard]] uint32_t width() const { return width_; }
    [[nodiscard]] uint32_t height() const { return height_; }
    [[nodiscard]] const std::string& font_name() const { return font_name_; }
    [[nodiscard]] uint32_t text_limit() const { return create_color_value_; }
    void commit_text(std::string text) { window_text_ = std::move(text); }
    void finish_edit(bool tab);
    [[nodiscard]] bool permits_replacement(std::string_view current,
        uint32_t first_utf16, uint32_t length_utf16, std::string_view insertion,
        bool direct_typing) const;
    [[nodiscard]] static uint32_t utf16_length(std::string_view text);
    [[nodiscard]] static std::string encode_engine_character(
        uint32_t codepoint, TextEncoding encoding);
    enum class HostChange { Create, Destroy, Style, Visibility };
    using HostObserver = std::function<void(HostChange)>;
    void set_host_observer(HostObserver observer) { host_observer_ = std::move(observer); }

private:
    bool open_ = false;
    bool visible_ = false;
    bool focus_child_ = false;
    uint32_t x_ = 0;
    uint32_t y_ = 0;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    std::string font_name_;
    uint32_t font_size_ = 100;
    uint32_t edit_font_size_ = 0;
    uint32_t create_color_value_ = 0;
    uint32_t color_bgr_ = 0xFFFFFF;
    std::string buffer_;
    std::string window_text_;
    uint32_t notification_value_ = 0;
    uint32_t secondary_value_ = 0;
    uint32_t selection_mode_ = 0;
    uint32_t active_alignment_ = 0;
    uint32_t active_width_percent_ = 100;
    HostObserver host_observer_;
};

}  // namespace bgiplayer
