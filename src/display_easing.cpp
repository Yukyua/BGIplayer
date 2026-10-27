#include "bgiplayer/display_easing.hpp"
#include <cmath>
#include <array>
#include <limits>
namespace bgiplayer {
namespace {
int32_t easing_truncate(double value) {
    // Original CVTTSD2SI's indefinite result, including fractional powers of
    // negative phases and values outside the signed dword range.
    if (!(value>=-2147483648.0 && value<2147483648.0))
        return std::numeric_limits<int32_t>::min();
    return static_cast<int32_t>(value);
}
}
std::optional<int32_t> evaluate_display_easing(int32_t phase, uint32_t selector) {
    if (selector-1U>=15U) return phase/256; //0041FAF3 signed truncation
    if (selector==1U) {
        // PE64 14005709D..70E7; PE32 0041F6BA..F705. Angle conversion
        // truncates the signed Q24 product before cosine, retaining Q16
        // degrees rather than directly evaluating phase*pi/2^24.
        const int64_t scaled = (static_cast<int64_t>(phase) * 180) / 256;
        const int32_t angle = static_cast<int32_t>(0xB40000U - static_cast<uint32_t>(scaled));
        const double radians = (static_cast<double>(angle) * 3.141592653589793) / 11796480.0;
        return easing_truncate((std::cos(radians) + 1.0) * 32768.0);
    }
    if (selector==2U || selector==3U) {
        // 1400570FA / 140057145 retain the truncated Q16 degree angle.
        const int64_t scaled=(static_cast<int64_t>(phase)*90)/256;
        const int32_t angle=selector==2U ? static_cast<int32_t>(scaled) :
            static_cast<int32_t>(0x5A0000U-static_cast<uint32_t>(scaled));
        const double radians=(static_cast<double>(angle)*3.141592653589793)/11796480.0;
        const double sine=std::sin(radians);
        return easing_truncate((selector==2U?sine:1.0-sine)*65536.0);
    }
    // 1400571A3..573C0: alternating power-in / power-out selectors.
    // Preserve original POW, multiplication and division order.
    constexpr std::array<double,6> exponents{2.0,2.5,3.0,4.0,5.0,6.0};
    const double exponent=exponents[(selector-4U)/2U];
    const bool inverse=(selector&1U)!=0;
    const int32_t input=inverse ? static_cast<int32_t>(0x1000000U-static_cast<uint32_t>(phase)) : phase;
    const double numerator=std::pow(static_cast<double>(input),exponent);
    const double denominator=std::pow(16777216.0,exponent);
    if (inverse) return easing_truncate((1.0-numerator/denominator)*65536.0);
    const volatile double scaled=numerator*65536.0;
    return easing_truncate(scaled/denominator);
}
}
