#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace bgiplayer {

enum class TextEncoding {
    ShiftJis,
    Utf8,
    SingleByte,
};

struct DecodedCharacter {
    uint32_t value = 0;
    std::size_t byte_length = 0;
    bool wide = false;
};

TextEncoding detect_text_encoding(std::string_view text);
DecodedCharacter decode_first_character(
    std::string_view text, TextEncoding encoding);
std::vector<uint32_t> decode_text_code_points(
    std::string_view text, TextEncoding encoding);
std::string encode_code_point(uint32_t value, TextEncoding encoding);
bool is_text_delimiter(uint32_t value, TextEncoding encoding);

}  // namespace bgiplayer
