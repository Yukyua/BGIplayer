#include "bgiplayer/gpu_bg_decode.hpp"
#include "bgiplayer/bmv_idct.hpp"
#include "bgiplayer/render_policy.hpp"
#include <algorithm>
#include <cfenv>
#include <cmath>
#include <cstdlib>
#include <cstring>
namespace bgiplayer {
#if !defined(__APPLE__)
std::shared_ptr<BgCoefficientWorkspace> allocate_host_background_coefficients(size_t) {return {};}
#endif
bool gpu_bg_decode_enabled(uint64_t pixels) {
    const char* enabled=std::getenv("BGI_GPU_DECODE");
    const char* gpu=std::getenv("BGI_GPU");
    return !retain_cpu_pixel_copies() && std::fegetround()==FE_TONEAREST &&
        !(enabled && std::strcmp(enabled,"0")==0) &&
        !(gpu && std::strcmp(gpu,"0")==0) &&
        ((gpu && std::strcmp(gpu,"1")==0) || pixels>=65536);
}
bool valid_gpu_bg_decode(const BgDecodePixels& p) {
    const uint64_t bx=(uint64_t(p.width)+7)/8,sy=(uint64_t(p.height)+7)/8;
    const auto quant=[](auto values){return std::all_of(values.begin(),values.end(),
        [](float f){return std::isfinite(f) && f>=0 && f<=512;});};
    return p.width && p.height && uint64_t(p.width)*p.height<=0x4000000 &&
        p.coefficients.size()==bx*sy*3 &&
        (p.alpha.empty() || p.alpha.size()==bx*sy*64) && quant(p.y_quant) && quant(p.c_quant);
}
namespace bg_decode_detail {
std::array<float,1152> tables(const BgDecodePixels& p) {
    std::array<float,1152> output;
    std::copy(p.y_quant.begin(),p.y_quant.end(),output.begin());
    std::copy(p.c_quant.begin(),p.c_quant.end(),output.begin()+64);
    const auto& colors=bmv_color_table_data();
    std::copy(colors.begin(),colors.end(),output.begin()+128);return output;
}
std::string_view math_source() {return
#include "bgiplayer/gpu_bg_decode_math.inc"
;}
std::string_view idct_source() {return
#include "bgiplayer/gpu_bg_decode_idct.inc"
;}
std::string_view color_source() {return
#include "bgiplayer/gpu_bg_decode_color.inc"
;}
}
}
