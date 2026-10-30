#include "bgiplayer/bmv_idct.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>

#if defined(__clang__)
#pragma clang fp contract(off)
#endif

namespace bgiplayer {
namespace {
// SHA-matched original PE 00500A20, not a regenerated cosine table.
constexpr std::array<uint32_t,64> scale_bits = {
    0x3F800000U, 0x3FB18A86U, 0x3FA73D75U, 0x3F968317U, 0x3F800000U, 0x3F49234EU, 0x3F0A8BD4U, 0x3E8D42AFU,
    0x3FB18A86U, 0x3FF641AFU, 0x3FE7F7F8U, 0x3FD0C429U, 0x3FB18A86U, 0x3F8B7E3FU, 0x3F402B4AU, 0x3EC3EF15U,
    0x3FA73D75U, 0x3FE7F7F8U, 0x3FDA827AU, 0x3FC4A74CU, 0x3FA73D75U, 0x3F836651U, 0x3F3504F3U, 0x3EB890D3U,
    0x3F968317U, 0x3FD0C429U, 0x3FC4A74CU, 0x3FB0FBC5U, 0x3F968317U, 0x3F6C835EU, 0x3F22E9C1U, 0x3EA61AD1U,
    0x3F800000U, 0x3FB18A86U, 0x3FA73D75U, 0x3F968317U, 0x3F800000U, 0x3F49234EU, 0x3F0A8BD4U, 0x3E8D42AFU,
    0x3F49234EU, 0x3F8B7E3FU, 0x3F836651U, 0x3F6C835EU, 0x3F49234EU, 0x3F1E0875U, 0x3ED9B5C9U, 0x3E5DF9ADU,
    0x3F0A8BD4U, 0x3F402B4AU, 0x3F3504F3U, 0x3F22E9C1U, 0x3F0A8BD4U, 0x3ED9B5C9U, 0x3E95F61AU, 0x3E18E635U,
    0x3E8D42AFU, 0x3EC3EF15U, 0x3EB890D3U, 0x3EA61AD1U, 0x3E8D42AFU, 0x3E5DF9ADU, 0x3E18E635U, 0x3D9BE50CU,
};
// Scalar lanes of original 004AB5C0 MULPS/ADDPS/SUBPS network.
// Explicit temporaries preserve each rounded SSE operation (no fused multiply-add).
std::array<float,8> inverse_lane(const std::array<float,8>& x) {
    constexpr float c1082 = std::bit_cast<float>(0x3F8A8BD4U);
    constexpr float sqrt2 = std::bit_cast<float>(0x3FB504F3U);
    constexpr float c1847 = std::bit_cast<float>(0x3FEC835EU);
    constexpr float cm2613 = std::bit_cast<float>(0xC0273D75U);
    const float sum26 = x[6] + x[2];
    const float sum04 = x[4] + x[0];
    const float delta26 = x[2] - x[6];
    const float scaled26 = delta26 * sqrt2;
    const float diff26 = scaled26 - sum26;
    const float diff04 = x[0] - x[4];
    const float even0 = sum26 + sum04;
    const float even3 = sum04 - sum26;
    const float even1 = diff26 + diff04;
    const float even2 = diff04 - diff26;
    const float diff53 = x[5] - x[3];
    const float sum53 = x[5] + x[3];
    const float sum71 = x[7] + x[1];
    const float diff17 = x[1] - x[7];
    const float oddtotal = sum71 + sum53;
    const float oddsum = diff17 + diff53;
    const float z5 = oddsum * c1847;
    const float product53 = cm2613 * diff53;
    const float oddfirst = product53 + z5;
    const float oddB = oddfirst - oddtotal;
    const float odddelta = sum71 - sum53;
    const float oddscaled = odddelta * sqrt2;
    const float oddC = oddscaled - oddB;
    const float product17 = diff17 * c1082;
    const float oddlast = product17 - z5;
    const float oddD = oddC + oddlast;
    return {oddtotal+even0,oddB+even1,oddC+even2,even3-oddD,
            oddD+even3,even2-oddC,even1-oddB,even0-oddtotal};
}
struct ColorTables {
    std::array<float,256> red_cr;
    std::array<float,256> green_cb;
    std::array<float,256> green_cr;
    std::array<float,256> blue_cb;
    ColorTables() {
        for (int i=0;i<256;++i) {
            const double centered = static_cast<double>(i-128);
            red_cr[static_cast<std::size_t>(i)] = static_cast<float>(centered * 1.402 + 0.5);
            green_cb[static_cast<std::size_t>(i)] = static_cast<float>(centered * -0.34414);
            green_cr[static_cast<std::size_t>(i)] = static_cast<float>(0.5 - centered * 0.71414);
            blue_cb[static_cast<std::size_t>(i)] = static_cast<float>(centered * 1.772 + 0.5);
        }
    }
};
uint8_t target_byte(float value) {
    const auto converted = static_cast<int32_t>(value);
    const auto packed = std::clamp(converted, -32768, 32767);
    return static_cast<uint8_t>(std::clamp(packed, 0, 255));
}
} // namespace

std::array<float,64> bmv_scaled_quantization(std::span<const uint8_t,64> quantization) {
    std::array<float,64> output{};
    for (std::size_t i=0;i<output.size();++i) {
        output[i] = static_cast<float>(quantization[i]) * std::bit_cast<float>(scale_bits[i]);
    }
    return output;
}

bool bmv_idct_block(const std::array<int16_t,64>& coefficients,
                    const std::array<float,64>& scaled_quantization,
                    std::array<int16_t,64>& samples) {
    if (std::any_of(scaled_quantization.begin(),scaled_quantization.end(),
                    [](float value){return !std::isfinite(value);})) return false;
    std::array<float,64> workspace{};
    for (std::size_t group=0;group<8;group+=4) {
        bool zero_ac=true;
        for (std::size_t row=1;row<8;++row)
            for (std::size_t col=group;col<group+4;++col)
                zero_ac = zero_ac && coefficients[row*8+col]==0;
        for (std::size_t col=group;col<group+4;++col) {
            std::array<float,8> column{};
            for (std::size_t row=0;row<8;++row)
                column[row]=static_cast<float>(coefficients[row*8+col])*scaled_quantization[row*8+col];
            if (zero_ac) column.fill(column[0]);
            else column=inverse_lane(column);
            for (std::size_t row=0;row<8;++row) workspace[row*8+col]=column[row];
        }
    }
    std::array<int16_t,64> output{};
    for (std::size_t row=0;row<8;++row) {
        std::array<float,8> lane{};
        std::copy_n(workspace.begin()+static_cast<std::ptrdiff_t>(row*8),8,lane.begin());
        lane=inverse_lane(lane);
        for (std::size_t col=0;col<8;++col) {
            const float value=lane[col];
            if (!std::isfinite(value) || static_cast<double>(value)<std::numeric_limits<int32_t>::min() ||
                static_cast<double>(value)>std::numeric_limits<int32_t>::max()) return false;
            const int32_t packed=std::clamp(static_cast<int32_t>(value),-32768,32767);
            output[row*8+col]=static_cast<int16_t>(std::clamp((packed>>3)+128,0,255));
        }
    }
    samples=output;
    return true;
}

const std::array<float,1024>& bmv_color_table_data() {
    static const auto data=[] {
        const ColorTables original;
        std::array<float,1024> output;
        std::copy(original.red_cr.begin(),original.red_cr.end(),output.begin());
        std::copy(original.green_cb.begin(),original.green_cb.end(),output.begin()+256);
        std::copy(original.green_cr.begin(),original.green_cr.end(),output.begin()+512);
        std::copy(original.blue_cb.begin(),original.blue_cb.end(),output.begin()+768);
        return output;
    }();
    return data;
}

bool bmv_ycbcr_to_bgr(int16_t y, int16_t cb, int16_t cr,
                      std::array<uint8_t,3>& bgr) {
    if (y<0 || y>255 || cb<0 || cb>255 || cr<0 || cr>255) return false;
    static const ColorTables tables;
    const auto cb_index = static_cast<std::size_t>(cb);
    const auto cr_index = static_cast<std::size_t>(cr);
    const float luma = static_cast<float>(y);
    // Keep target float addition order and table rounding visible.
    const float red = luma + tables.red_cr[cr_index];
    const float green_first = tables.green_cb[cb_index] + luma;
    const float green = green_first + tables.green_cr[cr_index];
    const float blue = tables.blue_cb[cb_index] + luma;
    bgr = {target_byte(blue),target_byte(green),target_byte(red)};
    return true;
}
} // namespace bgiplayer
