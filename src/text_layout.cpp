#include "bgiplayer/text_layout.hpp"
#include "bgiplayer/text_markup.hpp"
#include <algorithm>
#include <limits>

namespace bgiplayer {
namespace {
// Original PE UTF-16 table at 0x005102CC.
constexpr std::u16string_view initial = u"\u300c\u3000\uff08\u300e\x0022";
// Original PE UTF-16 table at 0x005102E0.
constexpr std::u16string_view run = u"\x0022\x0027\x002c\x002e\x003a\x003b\x003f\x0021\uff9e\uff9f\uff65\uff0c\uff0e\u3001\u3002\uff1a\uff1b\uff1f\uff01\u201d\u309b\u309c\u2010\x005d\x007d\x0029\uff09\u3015\uff3d\uff5d\u3009\u226b\u300b\u300d\u300f\u3011\u30fd\u30fe\u309d\u309e\u3005\u30fb\u2025\u2026\u2501\u2015\u2500\u30fc\uff5e\u266a\u3041\u3043\u3045\u3047\u3049\u3063\u3083\u3085\u3087\u30a1\u30a3\u30a5\u30a7\u30a9\u30c3\u30e3\u30e5\u30e7\u3092\u30f2\u3000";
// Original PE UTF-16 table at 0x00510370.
constexpr std::u16string_view opening = u"\x005b\x007b\x0028\uff08\u3014\uff3b\uff5b\u3008\u226a\u300a\u300c\u300e\u3010\u201c";
// Original PE UTF-16 table at 0x00510390.
constexpr std::u16string_view line_start = u"\x0022\x0027\x002c\x002e\x003f\x0021\uff9e\uff9f\uff0c\uff0e\u3001\u3002\uff1f\uff01\u201d\u309b\u309c\x005d\x007d\x0029\uff09\u3015\uff3d\uff5d\u3009\u226b\u300b\u300d\u300f\u3011\u30fd\u30fe\u309d\u309e\u3005\u30fb\u2025\u2026\u2501\u2015\u2500\u30fc\uff5e\u266a\u3041\u3043\u3045\u3047\u3049\u3063\u3083\u3085\u3087\u30a1\u30a3\u30a5\u30a7\u30a9\u30c3\u30e3\u30e5\u30e7\u3000";
bool member(std::u16string_view table, char16_t c) { return table.find(c) != table.npos; }
int32_t add(int32_t a, int32_t b) { return static_cast<int32_t>(static_cast<uint32_t>(a) + static_cast<uint32_t>(b)); }
} // namespace
bool target_wide_character(uint32_t c) { return c > 0x7FU && (c < 0xFF61U || c > 0xFF9FU); }
bool target_kinsoku_line_start(char16_t c) { return member(line_start, c); }
bool target_kinsoku_opening(char16_t c) { return member(opening, c); }
bool target_initial_hanging_character(char16_t c) { return member(initial, c); }
std::size_t target_kinsoku_run(std::u16string_view text) {
    std::size_t count = 0;
    while (count < text.size() && member(run, text[count])) ++count;
    return count;
}
std::size_t target_ascii_word_run(std::u16string_view text) {
    std::size_t count = 0;
    while (count < text.size() && ((text[count] > 0x20 && text[count] < 0x3C) ||
           static_cast<uint16_t>(text[count] - 0x3D) < 0x42U)) ++count;
    while (count > 1 && std::u16string_view(u"!\"),.?]}").find(text[count - 1]) != std::u16string_view::npos) --count;
    return count;
}

PlainTextLayoutResult layout_plain_fixed_text(
    std::u16string_view text, const PlainTextLayoutParameters& p,
    const GlyphFontRequest& font, const GlyphOutlineProvider& glyphs) {
    if (p.font_cell_width <= 0 || p.bounds_right < p.bounds_left ||
        p.bounds_right == std::numeric_limits<int32_t>::max()) {
        return {PlainTextLayoutError::InvalidState, std::nullopt};
    }
    std::optional<std::u16string> fallback;
    if (has_complete_text_markup(text)) {
        fallback = fallback_unsupported_text_markup(text);
        if (!fallback) return {PlainTextLayoutError::UnsupportedControlOrTag, std::nullopt};
        text = *fallback;
    }
    // These leading prefixes mutate parser state in original0043AD50.
    // Their unimplemented branches must not be mistaken for ignored body codes.
    if (!text.empty() && ((text.front() >= 2 && text.front() <= 8) || text.front() == 0x0B))
        return {PlainTextLayoutError::UnsupportedControlOrTag, std::nullopt};
    PlainTextLayout result{{}, p.cursor_x, p.cursor_y, 1};
    uint32_t delay = 0;
    bool after_line_break = false;
    for (std::size_t index = 0; index < text.size(); ++index) {
        uint32_t c = text[index];
        if (c == 0) break;
        if (c < 0xEU && c != '\n') {
            after_line_break = false;
            continue;
        }
        if (c == '\n') {
            result.cursor_x = p.bounds_left;
            result.cursor_y = add(result.cursor_y, p.line_advance);
            ++result.lines;
            after_line_break = true;
            continue;
        }
        if (c >= 0xD800U && c <= 0xDBFFU && index + 1U < text.size() &&
            text[index + 1U] >= 0xDC00 && text[index + 1U] <= 0xDFFF) {
            c = ((c - 0xD800U) << 10U) + text[++index] - 0xDC00U + 0x10000U;
        }
        if (p.ascii_word_grouping && target_ascii_word_run(text.substr(index + 1U)) != 0) {
            return {PlainTextLayoutError::UnsupportedControlOrTag, std::nullopt};
        }
        const int32_t advance = target_wide_character(c) ? p.font_cell_width : p.font_cell_width / 2;
        // 43D0xx: right edge is inclusive. Spacing is added after placement,
        // not included in the current glyph's wrap test in this style-0 path.
        if (add(result.cursor_x, advance) > p.bounds_right + 1) {
            result.cursor_x = p.bounds_left;
            result.cursor_y = add(result.cursor_y, p.line_advance);
            ++result.lines;
            after_line_break = true;
        }
        const uint32_t space = target_wide_character(c) ? 0x3000U : 0x20U;
        if (p.trim_line_leading_space && after_line_break && c == space) {
            after_line_break = false;
            continue;
        }
        auto glyph = load_glyph_outline(glyphs, font, c);
        if (!glyph) return {PlainTextLayoutError::MissingGlyph, std::nullopt};
        const uint64_t bytes = static_cast<uint64_t>(glyph->width) * glyph->height * 4U;
        if (glyph->width > std::numeric_limits<uint32_t>::max() / 4U ||
            bytes > std::numeric_limits<std::size_t>::max()) {
            return {PlainTextLayoutError::InvalidState, std::nullopt};
        }
        PlainTextGlyphNode node{c, result.cursor_x, add(result.cursor_y, p.baseline_offset),
            delay, 0, p.fade_duration, 0, glyph->width, glyph->height,
            std::vector<uint8_t>(static_cast<std::size_t>(bytes))};
        GlyphBitmapView view{node.bitmap, 0, glyph->width * 4U, glyph->width, glyph->height, 2, 4};
        if (!colorize_glyph_mask(view, *glyph, p.color)) {
            return {PlainTextLayoutError::InvalidState, std::nullopt};
        }
        result.nodes.push_back(std::move(node));
        result.cursor_x = add(result.cursor_x, add(p.character_spacing, advance));
        delay += p.character_delay;
        after_line_break = false;
    }
    return {PlainTextLayoutError::None, std::move(result)};
}
} // namespace bgiplayer
