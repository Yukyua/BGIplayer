#pragma once
#include <cstdint>
#include <span>
namespace bgiplayer {
// Original0040C210 pair RCPPS corrections, shared by scalar and GPU paths.
std::span<const uint8_t> original_layer_reciprocal_corrections();
}
