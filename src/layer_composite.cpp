#include "bgiplayer/layer_composite.hpp"
#include <array>
namespace bgiplayer {
std::span<const uint8_t> original_layer_reciprocal_corrections() {
    static const auto corrections=[] {
        std::array<uint8_t,65536> result{};
        constexpr uint32_t entries[]={
#include "original_mode0_alpha_corrections.inc"
        };
        for(const uint32_t entry:entries)result[entry>>3U]=static_cast<uint8_t>(entry&7U);
        return result;
    }();
    return corrections;
}
}
