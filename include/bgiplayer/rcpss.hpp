#pragma once

#include "bgiplayer/rcpss_estimate_table.hpp"

#include <bit>
#include <cstdint>
#include <limits>

#if defined(__x86_64__) || defined(__i386__)
#include <xmmintrin.h>
#endif

namespace bgiplayer::detail {

inline float rcpss_estimate_normal(float value) {
    const uint32_t bits = std::bit_cast<uint32_t>(value);
    const uint32_t exponent = (bits >> 23U) & 0xFFU;
#if defined(__x86_64__) || defined(__i386__)
    const __m128 input = _mm_set_ss(value);
    return _mm_cvtss_f32(_mm_rcp_ss(input));
#else
    const uint32_t pattern = (bits >> 11U) & 0xFFFU;
    const uint32_t estimate =
        kRcpSsEstimateTable[pattern] -
        ((exponent - 127U) << 23U);
    return std::bit_cast<float>(estimate);
#endif
}

inline float rcpss_estimate(float value) {
    const uint32_t bits = std::bit_cast<uint32_t>(value);
    const uint32_t sign = bits >> 31U;
    const uint32_t exponent = (bits >> 23U) & 0xFFU;
    const uint32_t mantissa = bits & 0x7FFFFFU;
    if (exponent == 0xFFU) {
        if (mantissa != 0) {
            return std::numeric_limits<float>::quiet_NaN();
        }
        return sign != 0 ? -0.0F : 0.0F;
    }
    if (exponent == 0U) {
        if (mantissa == 0U) {
            return sign != 0
                       ? -std::numeric_limits<float>::infinity()
                       : std::numeric_limits<float>::infinity();
        }
        const uint32_t highest_bit =
            31U - static_cast<uint32_t>(std::countl_zero(mantissa));
        const uint32_t normalized_mantissa =
            mantissa << (22U - highest_bit);
        const uint32_t estimate = static_cast<uint32_t>(
            static_cast<int64_t>(
                kRcpSsEstimateTable[normalized_mantissa >> 11U]) +
            (static_cast<int64_t>(149U - highest_bit) << 23U));
        return std::bit_cast<float>(estimate ^ (sign << 31U));
    }
    return std::bit_cast<float>(
        std::bit_cast<uint32_t>(rcpss_estimate_normal(value)) |
        (sign << 31U));
}

}  // namespace bgiplayer::detail
