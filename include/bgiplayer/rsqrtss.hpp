#pragma once

#include "bgiplayer/rsqrtss_estimate_table.hpp"

#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>

#if defined(__x86_64__) || defined(__i386__)
#include <xmmintrin.h>
#endif

namespace bgiplayer::detail {

inline float rsqrtss_estimate_normal(float value) {
    const uint32_t bits = std::bit_cast<uint32_t>(value);
    const uint32_t exponent = (bits >> 23U) & 0xFFU;
#if defined(__x86_64__) || defined(__i386__)
    const __m128 input = _mm_set_ss(value);
    return _mm_cvtss_f32(_mm_rsqrt_ss(input));
#else
    const uint32_t parity = exponent & 1U;
    const uint32_t pattern = (bits >> 11U) & 0xFFFU;
    const int32_t base_exponent = 126 + static_cast<int32_t>(parity);
    const int32_t shift =
        (static_cast<int32_t>(exponent) - base_exponent) / 2;
    const uint32_t estimate = static_cast<uint32_t>(
        static_cast<int32_t>(kRsqrtSsEstimateTable[parity][pattern]) -
        (shift << 23));
    return std::bit_cast<float>(estimate);
#endif
}

inline float rsqrtss_estimate(float value) {
    const uint32_t bits = std::bit_cast<uint32_t>(value);
    const uint32_t sign = bits >> 31U;
    const uint32_t exponent = (bits >> 23U) & 0xFFU;
    const uint32_t mantissa = bits & 0x7FFFFFU;
    if (exponent == 0xFFU) {
        if (mantissa != 0) {
            return std::numeric_limits<float>::quiet_NaN();
        }
        return sign != 0 ? std::numeric_limits<float>::quiet_NaN() : 0.0F;
    }
    if (exponent == 0) {
        if (mantissa == 0) {
            return sign != 0 ? -std::numeric_limits<float>::infinity()
                             : std::numeric_limits<float>::infinity();
        }
        return rsqrtss_estimate_normal(value * 16777216.0F) * 0x1p-12F;
    }
    if (sign != 0) {
        return std::numeric_limits<float>::quiet_NaN();
    }
    if (exponent > 225U) {
        return std::ldexp(rsqrtss_estimate_normal(value * 0x1p-64F),
                          32.0F);
    }
    return rsqrtss_estimate_normal(value);
}

}  // namespace bgiplayer::detail
