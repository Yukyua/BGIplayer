#include "bgiplayer/script.hpp"

#include "bgiplayer/common.hpp"

#include <string_view>

namespace bgiplayer {

ScriptCode parse_script(std::string name, std::span<const uint8_t> data) {
    if (data.size() < 8) {
        throw Error("script is too small: " + name);
    }
    const uint32_t code_offset = read_u32(data, 0);
    const uint32_t code_size = read_u32(data, 4);
    if (code_offset > data.size() ||
        static_cast<uint64_t>(code_offset) + code_size > data.size()) {
        throw Error("script code range is outside payload: " + name);
    }
    ScriptCode result{
        .name = std::move(name),
        .code_offset = code_offset,
        .code_size = code_size,
        .code = {},
    };
    result.code.assign(data.begin() + code_offset,
                       data.begin() + code_offset + code_size);
    return result;
}

}  // namespace bgiplayer
