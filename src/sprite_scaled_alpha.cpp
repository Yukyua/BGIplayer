#include "bgiplayer/sprite_scaled_alpha.hpp"
#include <array>
#include <bit>
#include <limits>
namespace bgiplayer {
namespace {
int32_t truncate_sse(double value) {
    if (!(value >= -2147483648.0 && value < 2147483648.0))
        return std::numeric_limits<int32_t>::min();
    return static_cast<int32_t>(value);
}
int32_t add_wrap(int32_t lhs, int32_t rhs) {
    return std::bit_cast<int32_t>(static_cast<uint32_t>(lhs) + static_cast<uint32_t>(rhs));
}
int floor_shift(int value, unsigned shift) {
    const int divisor = 1 << shift;
    return value >= 0 ? value / divisor : -int((-int64_t(value) + divisor - 1) / divisor);
}
}
bool compose_scaled_alpha_bgra(
    std::span<uint8_t> destination, uint32_t dw, uint32_t dh, uint32_t ds,
    std::span<const uint8_t> source, uint32_t sw, uint32_t sh, uint32_t ss,
    int32_t xq, int32_t yq, int32_t oxq, int32_t oyq,
    uint32_t sxq, uint32_t syq, uint32_t parameter, bool filtered) {
    if (sxq <= 32768 || syq <= 32768 || parameter > 256 ||
        sw > 32767 || sh > 32767 || dw > 32767 || dh > 32767 ||
        ds % 4 || ss % 4 || ss > 32764 || ds > 2147483647U || ds < uint64_t(dw) * 4 || ss < uint64_t(sw) * 4 ||
        destination.size() < uint64_t(ds) * dh || source.size() < uint64_t(ss) * sh)
        return false;
    // Keep the original 140052700 double operation order, including conversion
    // before row/column accumulation. Its SSE paddd increments wrap at 32 bits.
    const double inv_x = 65536.0 / static_cast<double>(sxq);
    const double inv_y = 65536.0 / static_cast<double>(syq);
    // Volatile temporaries prevent a host FMA from replacing the original
    // separate MULSD/SUBSD sequence.
    const volatile double translated_x = double(xq) / 65536.0 * inv_x;
    const volatile double translated_y = double(yq) / 65536.0 * inv_y;
    int32_t row_x = truncate_sse((double(oxq) / 65536.0 - translated_x) * 65536.0);
    int32_t row_y = truncate_sse((double(oyq) / 65536.0 - translated_y) * 65536.0);
    const int32_t dx = truncate_sse(inv_x * 65536.0);
    const int32_t dy = truncate_sse(inv_y * 65536.0);
    auto sample = [&](int x, int y, unsigned channel) -> int {
        if (x < 0 || y < 0 || uint32_t(x) >= sw || uint32_t(y) >= sh) return 0;
        return source[size_t(y) * ss + size_t(x) * 4 + channel];
    };
    if (!filtered) { row_x = add_wrap(row_x, 32768); row_y = add_wrap(row_y, 32768); }
    for (uint32_t y = 0; y < dh; ++y) {
        int32_t qx = row_x;
        for (uint32_t x = 0; x < dw; ++x) {
            const int ix = floor_shift(qx, 16), iy = floor_shift(row_y, 16);
            std::array<int, 4> rgba{};
            if (filtered) {
                const int fx = int((uint32_t(qx) & 65535U) >> 12);
                const int fy = int((uint32_t(row_y) & 65535U) >> 12);
                for (unsigned c = 0; c < 4; ++c) {
                    const int tl = sample(ix, iy, c), bl = sample(ix, iy + 1, c);
                    const int top = tl + floor_shift((sample(ix + 1, iy, c) - tl) * fx, 4);
                    const int bottom = bl + floor_shift((sample(ix + 1, iy + 1, c) - bl) * fx, 4);
                    rgba[c] = top + floor_shift((bottom - top) * fy, 4);
                }
            } else {
                for (unsigned c = 0; c < 4; ++c) rgba[c] = sample(ix, iy, c);
            }
            // LUT entries 0..126 use their index; entry 127 is explicitly
            // written from 128*(256-parameter), making alpha254/255 opaque.
            const int index = rgba[3] >> 1;
            const int weight = ((index == 127 ? 128 : index) * int(256 - parameter)) >> 8;
            const size_t offset = size_t(y) * ds + size_t(x) * 4;
            for (unsigned c = 0; c < 3; ++c) {
                const int old = destination[offset + c];
                destination[offset + c] = static_cast<uint8_t>(old + floor_shift((rgba[c] - old) * weight, 7));
            }
            qx = add_wrap(qx, dx);
        }
        row_y = add_wrap(row_y, dy);
    }
    return true;
}
}
