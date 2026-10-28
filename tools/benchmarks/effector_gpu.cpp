#include "bgiplayer/gpu_pixel_kernels.hpp"
#include "bgiplayer/effector_selectors.hpp"
#include "bgiplayer/effector_transform.hpp"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>
using namespace bgiplayer;
static void check(bool value,const char* why){if(!value)throw std::runtime_error(why);}
int main() {
    try {
        GpuPixelKernels gpu(true);check(gpu.available(),"GPU unavailable");
        std::cout<<"GPU_TEST_BACKEND device="<<gpu.device_name()<<'\n';
        std::ifstream blur("tests/fixtures/original_pe64_effector_selectors.bin",std::ios::binary);
        check(blur.good(),"original blur fixture");unsigned count=0;
        while(blur.peek()!=EOF) {
            uint32_t h[6],status;blur.read(reinterpret_cast<char*>(h),sizeof(h));
            const auto w=h[0],height=h[1],selector=h[2],r=h[3],fmt=h[5];
            check(w && w<=9 && height && height<=5 && selector<=5 && r<=256,"fixture bounds");
            std::vector<uint8_t> src(w*height*4),expected(src.size()),dst(src.size(),0xa5);
            blur.read(reinterpret_cast<char*>(src.data()),src.size());blur.read(reinterpret_cast<char*>(&status),4);
            blur.read(reinterpret_cast<char*>(expected.data()),expected.size());
            auto result=gpu.effector_blur({{dst,0,w*4,w,height,fmt,4},{src,0,w*4,w,height,fmt,4},r,selector});
            if(!result.success || !result.used_gpu || dst!=expected)std::cerr<<"blur case="<<count<<" selector="<<selector<<" radius="<<r<<'\n';
            check(blur.good() && !status && result.success && result.used_gpu && dst==expected,"GPU blur differs from original");++count;
        }
        check(count==3600,"blur case count");std::cout<<"GPU_EFFECTOR_BLUR_PASS cases="<<count<<'\n';
        std::ifstream displacement("tests/fixtures/original_pe64_effector_displacement.bin",std::ios::binary);
        check(displacement.good(),"original displacement fixture");count=0;
        while(displacement.peek()!=EOF) {
            uint32_t h[7],status;displacement.read(reinterpret_cast<char*>(h),sizeof(h));
            const auto w=h[0],height=h[1],fmt=h[2],strength=h[3],filtered=h[4],dual=h[5];
            check(w && w<=7 && height && height<=5 && strength<=256,"displacement fixture bounds");
            std::vector<uint8_t> src(w*height*4),a(src.size()),b(src.size()),expected(src.size()),dst(src.size(),0xa5);
            for(auto* bytes:{&src,&a,&b})displacement.read(reinterpret_cast<char*>(bytes->data()),bytes->size());
            displacement.read(reinterpret_cast<char*>(&status),4);displacement.read(reinterpret_cast<char*>(expected.data()),expected.size());
            const GlyphBitmapView second{b,0,w*4,w,height,4,4};
            auto result=gpu.effector_displacement({{dst,0,w*4,w,height,fmt,4},{src,0,w*4,w,height,fmt,4},
                {a,0,w*4,w,height,4,4},dual?std::optional(second):std::nullopt,strength,filtered!=0});
            if(!result.success || dst!=expected)std::cerr<<"displacement case="<<count<<'\n';
            check(displacement.good() && !status && result.success && result.used_gpu && dst==expected,"GPU displacement differs from original");++count;
        }
        check(count==2016,"displacement case count");std::cout<<"GPU_EFFECTOR_DISPLACEMENT_PASS cases="<<count<<'\n';
        std::ifstream vector("tests/fixtures/original_pe64_effector_vector.bin",std::ios::binary);
        check(vector.good(),"original vector fixture");count=0;
        while(vector.peek()!=EOF) {
            uint32_t h[6],status;std::array<uint32_t,8> coefficients;
            vector.read(reinterpret_cast<char*>(h),sizeof(h));vector.read(reinterpret_cast<char*>(coefficients.data()),sizeof(coefficients));
            const auto w=h[0],height=h[1],fmt=h[2],filtered=h[3],rectangular=h[5];
            check(w && w<=7 && height && height<=5,"vector fixture bounds");
            std::vector<uint8_t> src(w*height*4),map(w*height*6),expected(src.size()),dst(src.size(),0xa5);
            vector.read(reinterpret_cast<char*>(src.data()),src.size());vector.read(reinterpret_cast<char*>(map.data()),map.size());
            vector.read(reinterpret_cast<char*>(&status),4);vector.read(reinterpret_cast<char*>(expected.data()),expected.size());
            auto result=gpu.effector_vector({{dst,0,w*4,w,height,fmt,4},{src,0,w*4,w,height,fmt,4},
                {map,0,w*6,w,height,6,6},coefficients,filtered!=0,rectangular!=0});
            if(!result.success || dst!=expected)std::cerr<<"vector case="<<count<<'\n';
            check(vector.good() && !status && result.success && result.used_gpu && dst==expected,"GPU vector differs from original");++count;
        }
        check(count==288,"vector case count");std::cout<<"GPU_EFFECTOR_VECTOR_PASS cases="<<count<<'\n';
        {
            unsigned cases=0;
            for(uint32_t format:{1U,2U}) {
            std::ifstream temporal(format==1?"tests/fixtures/original_pe64_effector_temporal.bin":"tests/fixtures/original_pe64_effector_temporal_alpha.bin",std::ios::binary);
            check(temporal.good(),"original temporal fixture");
            while(temporal.peek()!=EOF) {
                uint32_t h[4];temporal.read(reinterpret_cast<char*>(h),sizeof(h));
                check(h[0] && h[0]<=9 && h[1] && h[1]<=5 && h[2]<=257,"temporal fixture bounds");
                std::vector<uint8_t> scene(h[0]*h[1]*4),history(scene.size()),want_scene(scene.size()),want_history(scene.size());
                for(auto* bytes:{&scene,&history,&want_scene,&want_history})temporal.read(reinterpret_cast<char*>(bytes->data()),bytes->size());
                const auto result=gpu.effector_temporal({{scene,0,h[0]*4,h[0],h[1],format,4},{history,0,h[0]*4,h[0],h[1],format,4},h[2]});
                check(temporal.good() && result.success && result.used_gpu && scene==want_scene && history==want_history,"GPU original consecutive temporal frames");++cases;
            }
            }
            check(cases==504,"temporal consecutive case count");std::cout<<"GPU_EFFECTOR_TEMPORAL_PASS cases="<<cases<<'\n';
        }
        {
            std::ifstream partial("tests/fixtures/original_pe64_effector_partial_kinds.bin",std::ios::binary);
            check(partial.good(),"original partial fixture");unsigned transforms=0,temporals=0,displacements=0;
            const int rects[][4]={{0,0,8,4},{1,1,6,3},{2,0,2,4},{0,2,8,2}};
            while(partial.peek()!=EOF) {
                uint32_t h[7];partial.read(reinterpret_cast<char*>(h),sizeof(h));
                std::vector<uint8_t> scene(1024),history(1024),want_scene(1024),want_history(1024);
                for(auto* bytes:{&scene,&history,&want_scene,&want_history})partial.read(reinterpret_cast<char*>(bytes->data()),bytes->size());
                check(partial.good() && h[0]>=1 && h[0]<=2 && h[1]<=4 && h[2]<4,"partial fixture bounds");
                if(h[1]!=0 && h[1]!=3 && h[1]!=4)continue;
                const auto& rect=rects[h[2]];
                const uint32_t offset=uint32_t(rect[1]*36+rect[0]*4),rw=uint32_t(rect[2]-rect[0]+1),rh=uint32_t(rect[3]-rect[1]+1);
                const uint32_t w=h[3]?rw:9,height=h[3]?rh:5,scene_offset=h[3]?offset:0;
                const GlyphBitmapView view{scene,scene_offset,36,w,height,h[0],4};
                if(h[1]!=4)for(uint32_t y=0;y<height;++y)std::copy_n(scene.data()+scene_offset+y*36,w*4,history.data()+y*36);
                if(h[1]==0) {
                    if(h[4]<=256) {
                        std::vector<uint8_t> map(9*5*4);
                        for(uint32_t y=0;y<5;++y)for(uint32_t x=0;x<9;++x) {
                            const std::array<int16_t,2> displacement{int16_t((int(x%3)-1)*32),int16_t((int(y%3)-1)*32)};
                            std::memcpy(map.data()+y*36+x*4,displacement.data(),4);
                        }
                        const GlyphBitmapView mv{map,0,36,9,5,4,4};
                        const auto result=gpu.effector_displacement({view,{history,0,36,9,5,h[0],4},mv,mv,h[4],h[5]!=0});
                        check(result.success && result.used_gpu,"GPU partial displacement dispatch");
                    }
                    check(scene==want_scene && history==want_history,"GPU original partial displacement source bounds");++displacements;
                } else
                if(h[1]==3) {
                    const auto matrix=effector_transform_matrix(9,5,{4*65536+16384,2*65536-8192,65536,72089,81920});
                    check(matrix.has_value(),"original transform matrix");
                    const auto result=gpu.effector_transform({view,{history,0,36,9,5,h[0],4},*matrix,h[5]!=0});
                    check(result.success && result.used_gpu && scene==want_scene && history==want_history,"GPU original partial affine callback");++transforms;
                } else {
                    const auto result=gpu.effector_temporal({view,{history,offset,36,rw,rh,h[0],4},h[4]});
                    check(result.success && result.used_gpu && scene==want_scene && history==want_history,"GPU original partial temporal callback and alpha pairing");++temporals;
                }
            }
            check(displacements==128 && transforms==128 && temporals==128,"partial displacement/transform/temporal case counts");
            std::cout<<"GPU_EFFECTOR_PARTIAL_PASS displacement="<<displacements<<" transform="<<transforms<<" temporal="<<temporals<<'\n';
        }
        {
            const std::vector<uint8_t> a(4099,0x39),b(65539,0x97);
            const std::array<std::span<const uint8_t>,2> input{a,b};
            const auto stored=gpu.store_pixels_batch(input);
            check(stored.size()==2,"GPU batch count");
            for(size_t n=0;n<input.size();++n) {
                std::vector<uint8_t> bytes(input[n].size());
                check(stored[n] && stored[n]->read(bytes) && std::equal(bytes.begin(),bytes.end(),input[n].begin()),"GPU batch bytes");
            }
            std::cout<<"GPU_BATCH_STORAGE_PASS cases=2\n";
        }
        for(auto [width,height]:{std::pair{1280U,720U},std::pair{1920U,1080U}}) {
            std::vector<uint8_t> source(width*height*4),cpu(source.size()),out(source.size());
            for(size_t n=0;n<source.size();++n)source[n]=uint8_t(n*37+17);
            const GlyphBitmapView input{source,0,width*4,width,height,1,4};
            for(bool accelerated:{false,true}) {
                std::vector<double> durations;
                for(int n=0;n<12;++n) {
                    auto begin=std::chrono::steady_clock::now();
                    if(accelerated)check(gpu.effector_blur({{out,0,width*4,width,height,1,4},input,3,5}).used_gpu,"benchmark GPU dispatch");
                    else check(blur_pe64_effector_selector({cpu,0,width*4,width,height,1,4},input,3,5),"CPU blur reference");
                    if(n>=2)durations.push_back(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count());
                }
                std::sort(durations.begin(),durations.end());
                std::cout<<"EFFECTOR_BENCH gpu="<<accelerated<<" width="<<width<<" height="<<height
                    <<" median_ms="<<durations[durations.size()/2]<<" max_ms="<<durations.back()<<'\n';
            }
            check(cpu==out,"full-size CPU/GPU result");
        }
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
