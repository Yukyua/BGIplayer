#pragma once
#include <cstdint>
#include <span>
#include <vector>
namespace bgiplayer {
// Original PE64 14008D3C0 raw row-difference container, before SDC wrapping.
// Requires nonzero dimensions and at least row_bytes*rows source bytes.
// Invalid host spans/dimensions throw Error; native status mapping is separate.
std::vector<uint8_t> encode_dcfs_payload(std::span<const uint8_t> input,
    uint32_t row_bytes, uint32_t rows);
}
