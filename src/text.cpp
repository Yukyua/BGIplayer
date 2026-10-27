#include "bgiplayer/text.hpp"

#include "bgiplayer/common.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <iconv.h>

namespace bgiplayer {
namespace {

std::size_t utf8_sequence_length(std::string_view text,
                                 std::size_t offset) {
    if (offset >= text.size()) {
        return 0;
    }
    const uint8_t first = static_cast<uint8_t>(text[offset]);
    if (first < 0x80U) {
        return 1;
    }
    std::size_t length = 0;
    if ((first & 0xE0U) == 0xC0U) {
        length = 2;
    } else if ((first & 0xF0U) == 0xE0U) {
        length = 3;
    } else if ((first & 0xF8U) == 0xF0U) {
        length = 4;
    } else {
        return 0;
    }
    if (offset + length > text.size()) {
        return 0;
    }
    for (std::size_t index = 1; index < length; ++index) {
        if ((static_cast<uint8_t>(text[offset + index]) & 0xC0U) !=
            0x80U) {
            return 0;
        }
    }
    return length;
}

uint32_t decode_utf8(std::string_view text, std::size_t offset,
                     std::size_t length) {
    const uint8_t first = static_cast<uint8_t>(text[offset]);
    if (length == 1) {
        return first;
    }
    uint32_t value = first & (0xFFU >> (length + 1U));
    for (std::size_t index = 1; index < length; ++index) {
        value = (value << 6U) |
                (static_cast<uint8_t>(text[offset + index]) & 0x3FU);
    }
    return value;
}

std::string encode_utf8(uint32_t value) {
    std::string out;
    if (value < 0x80U) {
        out.push_back(static_cast<char>(value));
    } else if (value < 0x800U) {
        out.push_back(static_cast<char>(0xC0U | (value >> 6U)));
        out.push_back(static_cast<char>(0x80U | (value & 0x3FU)));
    } else if (value < 0x10000U) {
        out.push_back(static_cast<char>(0xE0U | (value >> 12U)));
        out.push_back(static_cast<char>(
            0x80U | ((value >> 6U) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | (value & 0x3FU)));
    } else {
        out.push_back(static_cast<char>(0xF0U | (value >> 18U)));
        out.push_back(static_cast<char>(
            0x80U | ((value >> 12U) & 0x3FU)));
        out.push_back(static_cast<char>(
            0x80U | ((value >> 6U) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | (value & 0x3FU)));
    }
    return out;
}

std::string convert_encoding(std::string_view input,
                             const char* from, const char* to) {
    iconv_t converter = iconv_open(to, from);
    if (converter == reinterpret_cast<iconv_t>(-1)) {
        throw Error("cannot open text converter");
    }
    std::array<char, 32> output{};
    char* input_cursor =
        const_cast<char*>(input.data());
    std::size_t input_remaining = input.size();
    char* output_cursor = output.data();
    std::size_t output_remaining = output.size();
    const std::size_t result =
        iconv(converter, &input_cursor, &input_remaining,
              &output_cursor, &output_remaining);
    iconv_close(converter);
    if (result == static_cast<std::size_t>(-1) || input_remaining != 0) {
        throw Error("text conversion failed");
    }
    return std::string(
        output.data(),
        static_cast<std::size_t>(output_cursor - output.data()));
}

bool is_sjis_lead(uint32_t value) {
    return (value >= 0x81U && value <= 0x9FU) ||
           (value >= 0xE0U && value <= 0xFCU);
}

}  // namespace

TextEncoding detect_text_encoding(std::string_view text) {
    bool has_multibyte = false;
    bool valid_utf8 = true;
    for (std::size_t offset = 0; offset < text.size();) {
        const std::size_t length =
            utf8_sequence_length(text, offset);
        if (length == 0) {
            valid_utf8 = false;
            break;
        }
        has_multibyte = has_multibyte || length > 1;
        offset += length;
    }
    if (!valid_utf8) {
        return TextEncoding::ShiftJis;
    }
    return has_multibyte ? TextEncoding::Utf8 : TextEncoding::SingleByte;
}

DecodedCharacter decode_first_character(
    std::string_view text, TextEncoding encoding) {
    if (text.empty()) {
        return {};
    }
    const uint8_t first = static_cast<uint8_t>(text[0]);
    if (encoding == TextEncoding::SingleByte) {
        return {first, 1, false};
    }
    if (encoding == TextEncoding::Utf8) {
        const std::size_t length =
            utf8_sequence_length(text, 0);
        if (length == 0) {
            return {first, 1, false};
        }
        const uint32_t value = decode_utf8(text, 0, length);
        const bool wide =
            value > 0x7FU &&
            (value < 0xFF61U || value > 0xFF9FU);
        return {value, length, wide};
    }
    const bool two_byte = is_sjis_lead(first);
    const uint32_t value =
        two_byte && text.size() >= 2
            ? (static_cast<uint32_t>(first) << 8U) |
                  static_cast<uint8_t>(text[1])
            : first;
    return {value, two_byte && text.size() >= 2 ? 2U : 1U,
            two_byte};
}

std::vector<uint32_t> decode_text_code_points(
    std::string_view text, TextEncoding encoding) {
    std::vector<uint32_t> result;
    if (encoding == TextEncoding::ShiftJis) {
        iconv_t converter = iconv_open("UTF-32LE", "CP932");
        if (converter == reinterpret_cast<iconv_t>(-1)) {
            throw Error("cannot open Shift-JIS converter");
        }
        // Original Japanese PE32 4A0BE0 calls MultiByteToWideChar(932, 0).
        // Its CrossOver boundary consumes a lead plus its following byte even
        // for an invalid pair, returning U+30FB; iconv rejects those sequences.
        for (std::size_t position = 0; position < text.size();) {
            const uint8_t first = static_cast<uint8_t>(text[position]);
            if (first == 0x80U || first == 0xA0U || first >= 0xFDU) {
                result.push_back(first == 0x80U ? 0x80U :
                    first == 0xA0U ? 0xF8F0U : 0xF8F1U + first - 0xFDU);
                ++position;
                continue;
            }
            const std::size_t length = is_sjis_lead(first) &&
                    position + 1U < text.size() ? 2U : 1U;
            std::array<char, 8> output{};
            char* input_cursor = const_cast<char*>(text.data() + position);
            std::size_t input_remaining = length;
            char* output_cursor = output.data();
            std::size_t output_remaining = output.size();
            errno = 0;
            const std::size_t converted = iconv(
                converter, &input_cursor, &input_remaining,
                &output_cursor, &output_remaining);
            if (converted == static_cast<std::size_t>(-1) || input_remaining != 0) {
                if (errno != EILSEQ && errno != EINVAL) {
                    iconv_close(converter);
                    throw Error("Shift-JIS conversion failed");
                }
                result.push_back(0x30FBU);
            } else {
                const std::size_t bytes = static_cast<std::size_t>(
                    output_cursor - output.data());
                for (std::size_t offset = 0; offset + 3U < bytes; offset += 4U) {
                    result.push_back(
                        static_cast<uint32_t>(static_cast<uint8_t>(output[offset])) |
                        (static_cast<uint32_t>(static_cast<uint8_t>(output[offset + 1U])) << 8U) |
                        (static_cast<uint32_t>(static_cast<uint8_t>(output[offset + 2U])) << 16U) |
                        (static_cast<uint32_t>(static_cast<uint8_t>(output[offset + 3U])) << 24U));
                }
            }
            position += length;
        }
        iconv_close(converter);
        return result;
    }
    for (std::size_t offset = 0; offset < text.size();) {
        const DecodedCharacter decoded =
            decode_first_character(text.substr(offset), encoding);
        if (decoded.byte_length == 0) {
            break;
        }
        result.push_back(decoded.value);
        offset += decoded.byte_length;
    }
    return result;
}

std::string encode_code_point(uint32_t value, TextEncoding encoding) {
    if (encoding == TextEncoding::SingleByte) {
        return std::string(1, static_cast<char>(value & 0xFFU));
    }
    if (encoding == TextEncoding::Utf8) {
        return encode_utf8(value);
    }
    return convert_encoding(encode_utf8(value), "UTF-8", "CP932");
}

bool is_text_delimiter(uint32_t value, TextEncoding encoding) {
    static constexpr std::array<uint32_t, 36> sjis = {
        0x002C, 0x002E, 0x00A4, 0x00A1, 0x003A, 0x003B, 0x003F, 0x0021,
        0x00DE, 0x00DF, 0x00A5, 0x8141, 0x8142, 0x8143, 0x8144, 0x8146,
        0x8147, 0x8148, 0x8149, 0x814A, 0x814B, 0x815D, 0x005D, 0x007D,
        0x0029, 0x816A, 0x816C, 0x816E, 0x8170, 0x8172, 0x8174, 0x8176,
        0x8178, 0x817A, 0x8165, 0x8167,
    };
    static constexpr std::array<uint32_t, 36> unicode = {
        0x002C, 0x002E, 0xFF64, 0xFF61, 0x003A, 0x003B, 0x003F, 0x0021,
        0xFF9E, 0xFF9F, 0xFF65, 0x3001, 0x3002, 0xFF0C, 0xFF0E, 0xFF1A,
        0xFF1B, 0xFF1F, 0xFF01, 0x309B, 0x309C, 0x2010, 0x005D, 0x007D,
        0x0029, 0xFF09, 0x3015, 0xFF3D, 0xFF5D, 0x3009, 0x300B, 0x300D,
        0x300F, 0x3011, 0x2018, 0x201C,
    };
    const auto& table =
        encoding == TextEncoding::ShiftJis ? sjis : unicode;
    return std::find(table.begin(), table.end(), value) != table.end();
}

}  // namespace bgiplayer
