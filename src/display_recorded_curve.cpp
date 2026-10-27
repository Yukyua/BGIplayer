#include "bgiplayer/display_recorded_curve.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#if defined(__clang__)
#pragma clang fp contract(off)
#endif
namespace bgiplayer {
namespace {
int32_t truncate_sse(double v) {
    if(!(v>=-2147483648.0 && v<2147483648.0))return std::numeric_limits<int32_t>::min();
    return static_cast<int32_t>(v);
}
double product(double a,double b) {
    // Keep separate MULSD and ADDSD/SUBSD on hosts that contract by default.
    volatile double value=a*b;return value;
}
}
std::optional<std::array<uint32_t,3>> evaluate_recorded_display_curve(
    std::span<const std::array<uint32_t,3>> points,uint32_t factor,uint32_t extent) {
    if(points.empty() || extent==0)return std::nullopt;
    points=points.first(std::min<size_t>(100,points.size())); // 140087F80
    if(factor>=extent || points.size()==1)return points.back();
    const double t=double(factor)/double(extent);
    std::array<uint32_t,3> output{};
    for(size_t axis=0;axis<3;++axis) {
        if(points.size()==2) {
            const int32_t delta=static_cast<int32_t>(points[1][axis]-points[0][axis]);
            output[axis]=points[0][axis]+uint32_t(truncate_sse(product(double(delta),t)));
            continue;
        }
        const size_t n=points.size()-1;
        std::array<double,100> value{},linear{},quadratic{},cubic{},work{};
        for(size_t i=0;i<=n;++i)value[i]=double(static_cast<int32_t>(points[i][axis]));
        for(size_t i=1;i<n;++i)
            quadratic[i]=product((value[i-1]-(value[i]+value[i]))+value[i+1],3.0);
        for(size_t i=1;i<n;++i) {
            const double divisor=4.0-work[i-1];
            quadratic[i]=(quadratic[i]-quadratic[i-1])/divisor;
            work[i]=1.0/divisor;
        }
        for(size_t i=n-1;i>0;--i)quadratic[i]=quadratic[i]-product(work[i],quadratic[i+1]);
        for(size_t i=0;i<n;++i) {
            cubic[i]=(quadratic[i+1]-quadratic[i])/3.0;
            linear[i]=((value[i+1]-value[i])-quadratic[i])-cubic[i];
        }
        const double position=product(double(n),t);
        const size_t segment=std::min(n-1,static_cast<size_t>(std::floor(position)));
        const double fraction=position-double(segment);
        const double a=product(fraction,cubic[segment])+quadratic[segment];
        const double b=product(a,fraction)+linear[segment];
        output[axis]=uint32_t(truncate_sse(product(b,fraction)+value[segment]));
    }
    return output;
}
}
