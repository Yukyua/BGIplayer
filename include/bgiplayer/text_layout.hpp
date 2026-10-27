#pragma once
#include "bgiplayer/glyph.hpp"
#include <optional>
#include <string_view>

namespace bgiplayer {

// The unchanged-font/style-0, horizontal fixed-cell branch of 0043AD50.
// All scalars come from target window/font/global state, not host advances.
struct PlainTextLayoutParameters {
    int32_t cursor_x;
    int32_t cursor_y;
    int32_t bounds_left;
    int32_t bounds_right; // inclusive
    int32_t font_cell_width; // 0043D890: font.+64 * font.+60 / 100
    int32_t character_spacing; // 0043D800(0,font)
    int32_t line_advance; // fontHeight + fontHeight*windowLineSpacingPercent/100; 43D750=0
    int32_t baseline_offset; // zero when parser param4 == 0
    uint32_t character_delay; // 00529C04
    uint32_t fade_duration; // 00529BEC
    uint32_t color;
    bool trim_line_leading_space; // 0053321C
    bool ascii_word_grouping; // parser local1E10 (prefix 0x0B clears it)
    float font_spacing = 0; // PE64 font-cache +4C, separate from fixed integer gap.
};

struct PlainTextGlyphNode {
    uint32_t codepoint;
    int32_t x;
    int32_t y;
    uint32_t start_delay;
    uint32_t elapsed;
    uint32_t fade_duration;
    uint32_t kind;
    uint32_t width;
    uint32_t height;
    std::vector<uint8_t> bitmap; // original format-2 BGRA bytes
};

struct TextRubyAlias {
    std::u16string base;
    std::u16string reading;
};
// 1400786F0: per-call base\\reading\n dictionary, later duplicate replaces.
std::optional<std::vector<TextRubyAlias>> parse_text_ruby_dictionary(std::u16string_view text);

struct TextRubySpan {std::size_t first;std::size_t count;std::u16string reading;};
struct ParsedRubyText {
    std::u16string visible;
    std::vector<TextRubySpan> ruby;
    std::vector<std::size_t> boundaries;
};
// PE32 43AD50 and PE64 75570 both lowercase R headers and register the next base run.
std::optional<ParsedRubyText> parse_text_ruby(std::u16string_view text,
    std::span<const TextRubyAlias> aliases);

struct PlainTextRubyRun {
    std::size_t first_node; // Auxiliary chain is inserted after this anchor.
    std::u16string reading;
    int32_t x;
    int32_t y;
    int32_t width; // 1400796C0 output+4 for the matched base span.
    uint32_t base_characters;
    uint32_t start_delay;
};

struct PlainTextLayout {
    std::vector<PlainTextGlyphNode> nodes;
    int32_t cursor_x;
    int32_t cursor_y;
    uint32_t lines;
    std::vector<PlainTextRubyRun> ruby_runs;
};

enum class PlainTextLayoutError { None, UnsupportedControlOrTag, MissingGlyph, InvalidState };
struct PlainTextLayoutResult {
    PlainTextLayoutError error;
    std::optional<PlainTextLayout> layout;
};

// Input is already target-decoded UTF-16. Aliases/tags, proportional metrics,
// ruby and param11 kinsoku mode are separate frontend branches and
// must not be selected by the caller for this fixed-cell entry point. When ASCII
// grouping is enabled, a lookahead run returns an explicit unsupported result.
PlainTextLayoutResult layout_plain_fixed_text(
    std::u16string_view text, const PlainTextLayoutParameters& parameters,
    const GlyphFontRequest& font, const GlyphOutlineProvider& glyphs);

bool target_wide_character(uint32_t codepoint); // 004A1240
bool target_kinsoku_line_start(char16_t character); // 43E590/43E5E0
bool target_kinsoku_opening(char16_t character); // 43E590/43E5F0
bool target_initial_hanging_character(char16_t character); // 43E5D0
std::size_t target_kinsoku_run(std::u16string_view text); // 43E530
std::size_t target_ascii_word_run(std::u16string_view text); // 43E600

} // namespace bgiplayer

namespace bgiplayer {
// Explicit scalars for the plain, unchanged-font/style0 branch of 0043AD50.
// Ruby enable can reserve baseline/indent even when no auxiliary alias matches;
// caller supplies that baseline in plain.baseline_offset and indent here.
struct FixedKinsokuLayoutParameters {
    PlainTextLayoutParameters plain;
    int32_t font_height;
    int32_t reserved_right_margin; // 529BE8 * fontHeight when kinsoku enabled
    int32_t line_indent; // 5328C0 when ruby enabled, else0
    bool kinsoku;
    bool allow_reserved_margin_for_punctuation; // 529BF4
    bool initial_hanging_indent; // 5328D0, no prefix overriding initial character
    int32_t style_wrap_extension = 0; // 43D0xx adds style1/2 horizontal offset after grouping
};
// PE32 0043AD50 parser with independent legacy ABC rounding and kinsoku tables.
PlainTextLayoutResult layout_legacy_text(std::u16string_view text,
    const FixedKinsokuLayoutParameters& parameters,const GlyphFontRequest& font,
    const GlyphOutlineProvider& glyphs,bool proportional,uint32_t reciprocal,
    uint32_t right_extension=0,std::vector<GlyphMask>* style_sources=nullptr,
    bool ruby_enabled=false,std::span<const TextRubyAlias> aliases={});
// Fixed-cell entry, retaining the same legacy kinsoku tables.
// ASCII word grouping and Japanese kinsoku are the recovered original branches.
PlainTextLayoutResult layout_plain_fixed_kinsoku_text(
    std::u16string_view text,const FixedKinsokuLayoutParameters& parameters,
    const GlyphFontRequest& font,const GlyphOutlineProvider& glyphs);
} // namespace bgiplayer

namespace bgiplayer {
// Recovered PE64 style0 proportional branch, single unwrapped line.
// Unsupported wrapping/kinsoku/ruby remain explicit caller gates.
PlainTextLayoutResult layout_plain_proportional_line(
    std::u16string_view text,const PlainTextLayoutParameters& parameters,
    const GlyphFontRequest& font,const GlyphOutlineProvider& glyphs,uint32_t reciprocal);
}

namespace bgiplayer {
// PE64 140075570 horizontal parser: proportional ABC, grouping and kinsoku.
// Explicit R tags retain ruby metadata for the separate 140075140 pass;
// dictionary aliases, other control tags and transformed fonts remain separate.
// Same PE64 parser with fontfield5=0: full glyph cache canvas, fixed
// wide/half cells, shared PE64 grouping/kinsoku, no proportional ink crop.
PlainTextLayoutResult layout_pe64_fixed_text(
    std::u16string_view text,const FixedKinsokuLayoutParameters& parameters,
    const GlyphFontRequest& font,const GlyphOutlineProvider& glyphs,
    bool ruby_enabled=false, std::span<const TextRubyAlias> aliases = {},
    std::vector<GlyphMask>* style_sources=nullptr);
PlainTextLayoutResult layout_pe64_proportional_text(
    std::u16string_view text,const FixedKinsokuLayoutParameters& parameters,
    const GlyphFontRequest& font,const GlyphOutlineProvider& glyphs,uint32_t reciprocal,
    uint32_t right_extension,std::vector<GlyphMask>* style_sources=nullptr,
    bool ruby_enabled=false, std::span<const TextRubyAlias> aliases = {});

struct Pe64RubyLayoutParameters {
    int32_t cell_width;
    uint32_t reciprocal;
    float font_spacing;
    int32_t offset_x; // 1401D8C30
    int32_t offset_y; // 1401D8C34, before subtracting ruby font height.
    int32_t canvas_extension_x; // style1 radius, style2 double radius, else0.
    int32_t canvas_extension_y;
    uint32_t color;
    uint32_t character_delay;
    uint32_t fade_duration;
};
PlainTextLayoutResult layout_legacy_ruby_text(const PlainTextRubyRun& run,
    const Pe64RubyLayoutParameters& parameters,const GlyphFontRequest& font,
    const GlyphOutlineProvider& glyphs);
// 140075140 -> 140074A50 creates a separate kind2 chain immediately after
// the anchor base glyph. This helper leaves the main cursor unchanged.
PlainTextLayoutResult layout_pe64_ruby_text(const PlainTextRubyRun& run,
    const Pe64RubyLayoutParameters& parameters,const GlyphFontRequest& font,
    const GlyphOutlineProvider& glyphs);
}
