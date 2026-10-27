#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace bgiplayer {

bool is_dsc(std::span<const uint8_t> data);
std::vector<uint8_t> dsc_decode(std::span<const uint8_t> data);
std::vector<uint8_t> dsc_encode_sdc(std::span<const uint8_t> data);

}  // namespace bgiplayer
