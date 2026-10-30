#include "bgiplayer/compressed_bg_v2.hpp"
#include "bgiplayer/pixel_storage.hpp"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>
using Clock=std::chrono::steady_clock;
int main(int argc,char** argv) {
    using namespace bgiplayer;
    setenv("BGI_GPU","1",1);unsetenv("BGI_KEEP_CPU_PIXELS");unsetenv("BGI_GPU_DECODE");
    for(int arg=1;arg<argc;++arg) {
        std::ifstream f(argv[arg],std::ios::binary);
        const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(f),{}};
        std::string error;auto reference=decode_compressed_bg_v2(bytes,&error);
        if(!reference)throw std::runtime_error(error);
        const auto pack=[](const CompressedBgV2Decoded& p) {
            std::vector<uint8_t> out(size_t(p.width)*p.height*4);
            for(size_t n=0;n<size_t(p.width)*p.height;++n) {
                std::copy_n(p.pixels.data()+n*(p.bits_per_pixel/8),3,out.data()+n*4);
                out[n*4+3]=p.bits_per_pixel==32?p.pixels[n*4+3]:0;
            }
            return out;
        };
        const auto gold=pack(*reference);
        const auto first=Clock::now();auto warm=decode_compressed_bg_v2(bytes,&error,true);
        const double first_ms=std::chrono::duration<double,std::milli>(Clock::now()-first).count();
        if(!warm || !warm->resident_pixels)throw std::runtime_error("GPU decode did not dispatch");
        std::vector<double> cpu,gpu;
        for(unsigned run=0;run<11;++run)for(unsigned pass=0;pass<2;++pass) {
            const bool accelerated=((run+pass)%2)==0;
            const auto start=Clock::now();auto decoded=decode_compressed_bg_v2(bytes,&error,accelerated);
            if(!decoded)throw std::runtime_error(error);
            std::shared_ptr<GpuPixelStorage> storage=decoded->resident_pixels;
            if(!accelerated){auto internal=pack(*decoded);storage=store_host_pixel_storage(internal);}
            const double ms=std::chrono::duration<double,std::milli>(Clock::now()-start).count();
            if(!storage)throw std::runtime_error("background upload failed");
            std::vector<uint8_t> out(storage->size());
            if(!storage->read(out) || out!=gold)throw std::runtime_error("background GPU pixels differ");
            (accelerated?gpu:cpu).push_back(ms);
        }
        std::sort(cpu.begin(),cpu.end());std::sort(gpu.begin(),gpu.end());
        std::cout<<"BACKGROUND_DECODE bytes="<<bytes.size()<<" dimensions="<<reference->width<<'x'<<reference->height
            <<" first_gpu_ms="<<first_ms<<" cpu_prepare_median_ms="<<cpu[5]<<" gpu_prepare_median_ms="<<gpu[5]<<" exact=1\n";
    }
}
