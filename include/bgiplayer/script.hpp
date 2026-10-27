#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace bgiplayer {

struct ScriptCode {
    std::string name;
    uint32_t code_offset = 0;
    uint32_t code_size = 0;
    std::vector<uint8_t> code;
};

ScriptCode parse_script(std::string name, std::span<const uint8_t> data);

}  // namespace bgiplayer
