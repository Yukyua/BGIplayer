#pragma once
#include <cstdint>
#include <span>
#include <vector>
namespace bgiplayer {
// Original1400F5F80 command stream; the caller's milliseconds supply the mask seed.
std::vector<uint8_t> compress_original_sdc_payload(std::span<const uint8_t> input);
std::vector<uint8_t> encode_original_sdc(std::span<const uint8_t> input,uint16_t seed);
}
