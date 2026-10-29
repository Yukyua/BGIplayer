#include "bgiplayer/pixel_region.hpp"
#include "bgiplayer/gpu_pixel_cache.hpp"
#include "bgiplayer/gpu_exact_pixel_ops.hpp"
#include "bgiplayer/gpu_raw_patches.hpp"
#include <mutex>
#include <cstdlib>
#include <cstring>
namespace bgiplayer {
namespace {
class PixelPatch final : public GpuPixelStorage {
    mutable std::mutex mutex_;
    mutable std::shared_ptr<GpuPixelStorage> base_,patch_,concrete_;
    size_t bytes_,offset_,stride_,row_bytes_,rows_;
public:
    PixelPatch(std::shared_ptr<GpuPixelStorage> base,std::shared_ptr<GpuPixelStorage> patch,
        size_t offset,size_t stride,size_t row_bytes,size_t rows)
        :base_(std::move(base)),patch_(std::move(patch)),bytes_(base_->size()),
         offset_(offset),stride_(stride),row_bytes_(row_bytes),rows_(rows) {}
    size_t size() const override { return bytes_; }
    bool fast_ranges() const override {
        std::lock_guard lock(mutex_);
        return concrete_?concrete_->fast_ranges():base_->fast_ranges();
    }
    bool read_range(size_t offset,std::span<uint8_t> out) const override {
        if(offset>bytes_ || out.size()>bytes_-offset)return false;
        std::shared_ptr<GpuPixelStorage> base,patch,concrete;
        {std::lock_guard lock(mutex_);base=base_;patch=patch_;concrete=concrete_;}
        if(concrete)return concrete->read_range(offset,out);
        if(!base->read_range(offset,out))return false;
        if(out.empty() || offset+out.size()<=offset_)return true;
        const size_t first=offset>offset_?(offset-offset_)/stride_:0;
        const size_t last=std::min(rows_-1,(offset+out.size()-1-offset_)/stride_);
        for(size_t row=first;row<=last;++row) {
            const size_t start=offset_+row*stride_,left=std::max(start,offset);
            const size_t right=std::min(start+row_bytes_,offset+out.size());
            if(left<right && !patch->read_range(row*row_bytes_+left-start,out.subspan(left-offset,right-left)))return false;
        }
        return true;
    }
    bool read(std::span<uint8_t> out) const override {
        if(out.size()!=bytes_)return false;
        std::shared_ptr<GpuPixelStorage> base,patch,concrete;
        {std::lock_guard lock(mutex_);base=base_;patch=patch_;concrete=concrete_;}
        if(concrete)return concrete->read(out);
        if(!base->read(out))return false;
        std::vector<uint8_t> bytes(patch->size());
        if(!patch->read(bytes))return false;
        for(size_t row=0;row<rows_;++row)
            std::copy_n(bytes.data()+row*row_bytes_,row_bytes_,out.data()+offset_+row*stride_);
        return true;
    }
    std::shared_ptr<GpuPixelStorage> crop(size_t offset,size_t stride,size_t row_bytes,size_t rows) const {
        std::shared_ptr<GpuPixelStorage> base,patch,concrete;
        {std::lock_guard lock(mutex_);base=base_;patch=patch_;concrete=concrete_;}
        if(concrete)return crop_host_pixel_storage(concrete,offset,stride,row_bytes,rows);
        if(stride!=stride_ || offset%stride+row_bytes>stride || offset_%stride+row_bytes_>stride)return {};
        const size_t x=offset%stride,y=offset/stride,px=offset_%stride,py=offset_/stride;
        if(x>=px && row_bytes<=row_bytes_ && x-px<=row_bytes_-row_bytes &&
                y>=py && rows<=rows_ && y-py<=rows_-rows)
            return crop_host_pixel_storage(patch,(y-py)*row_bytes_+x-px,row_bytes_,row_bytes,rows);
        if(x+row_bytes<=px || px+row_bytes_<=x || y+rows<=py || py+rows_<=y)
            return crop_host_pixel_storage(base,offset,stride,row_bytes,rows);
        return {}; // A crossing crop uses the ordinary transactional flatten.
    }
    std::shared_ptr<GpuPixelStorage> concrete_storage() const override {
        std::shared_ptr<GpuPixelStorage> base,patch;
        {std::lock_guard lock(mutex_);if(concrete_)return concrete_;base=base_;patch=patch_;}
        std::shared_ptr<GpuPixelStorage> result;
        // Flatten immutable patch chains with one base copy and one ordered
        // command submission, instead of copying the full canvas per patch.
        std::vector<GpuRawPatch> patches{{patch,offset_,stride_,row_bytes_,rows_}};
        auto terminal=base;
        while(patches.size()<256) {
            const auto node=std::dynamic_pointer_cast<PixelPatch>(terminal);
            if(!node)break;
            std::lock_guard lock(node->mutex_);
            if(node->concrete_){terminal=node->concrete_;break;}
            patches.push_back({node->patch_,node->offset_,node->stride_,node->row_bytes_,node->rows_});
            terminal=node->base_;
        }
        if(patches.size()>1 && !std::dynamic_pointer_cast<PixelPatch>(terminal)) {
            std::reverse(patches.begin(),patches.end());
            result=compose_host_raw_patches(terminal,patches);
        }
        if(!result && bytes_<=UINT32_MAX && offset_<=UINT32_MAX && stride_<=UINT32_MAX &&
                row_bytes_%4==0 && rows_<=UINT32_MAX) {
            const auto w=static_cast<uint32_t>(row_bytes_/4),h=static_cast<uint32_t>(rows_);
            ExactPixelPixels p{{{},static_cast<uint32_t>(offset_),static_cast<uint32_t>(stride_),w,h,2,4},
                {{},0,static_cast<uint32_t>(row_bytes_),w,h,2,4},{},ExactPixelOperation::RawCopy};
            p.destination_storage=base;p.source_storage=patch;
            result=compose_host_resident_exact_pixels(p);
        }
        if(!result) {
            std::vector<uint8_t> bytes(bytes_);
            if(!read(bytes))return {};
            result=store_host_pixel_storage(bytes);
        }
        if(!result)return {};
        std::lock_guard lock(mutex_);
        if(!concrete_){concrete_=std::move(result);base_.reset();patch_.reset();}
        return concrete_;
    }
};
}
std::shared_ptr<GpuPixelStorage> crop_host_pixel_storage(
    std::shared_ptr<GpuPixelStorage> source,size_t offset,size_t stride,size_t row_bytes,size_t rows) {
    if(!source || !row_bytes || row_bytes%4 || !rows || offset>UINT32_MAX || stride>UINT32_MAX ||
            rows>UINT32_MAX || row_bytes>UINT32_MAX || rows>UINT32_MAX/row_bytes)return {};
    if(stride<row_bytes || offset>source->size() || (rows-1)>(source->size()-offset)/stride ||
            row_bytes>source->size()-offset-(rows-1)*stride)return {};
    const auto w=static_cast<uint32_t>(row_bytes/4),h=static_cast<uint32_t>(rows);
    if(!exact_pixels_worthwhile(w,h,true))return {};
    const std::array<std::shared_ptr<GpuPixelStorage>,1> inputs{source};
    const std::array<uint64_t,4> key{offset,stride,row_bytes,rows};
    if(auto cached=find_gpu_pixel_cache("crop",inputs,key))return cached;
    const auto publish=[&](std::shared_ptr<GpuPixelStorage> result) {
        if(result)store_gpu_pixel_cache("crop",inputs,key,result);
        return result;
    };
    const char* forced=std::getenv("BGI_GPU");
    if(!(forced && std::strcmp(forced,"1")==0) && offset==0 && stride==row_bytes &&
            source->size()==row_bytes*rows)return source;
    if(auto patched=std::dynamic_pointer_cast<PixelPatch>(source))
        if(auto result=patched->crop(offset,stride,row_bytes,rows))return publish(std::move(result));
    ExactPixelPixels p{{{},0,static_cast<uint32_t>(row_bytes),w,h,2,4},
        {{},static_cast<uint32_t>(offset),static_cast<uint32_t>(stride),w,h,2,4},{},ExactPixelOperation::RawCopy};
    p.destination_bytes=row_bytes*rows;p.source_storage=std::move(source);
    return publish(compose_host_resident_exact_pixels(p));
}
std::shared_ptr<GpuPixelStorage> patched_pixel_storage(std::shared_ptr<GpuPixelStorage> base,
    std::shared_ptr<GpuPixelStorage> patch,size_t offset,size_t stride,size_t row_bytes,size_t rows) {
    if(!base || !patch || !base->fast_ranges() || !patch->fast_ranges() || !rows ||
        !row_bytes || rows>SIZE_MAX/row_bytes || stride<row_bytes || patch->size()!=row_bytes*rows || offset>base->size() ||
        (rows-1)>(base->size()-offset)/stride || row_bytes>base->size()-offset-(rows-1)*stride)return {};
    const std::array<std::shared_ptr<GpuPixelStorage>,2> inputs{base,patch};
    const std::array<uint64_t,4> key{offset,stride,row_bytes,rows};
    // Small changing labels must not displace an entire scene's cached prefix.
    const bool memoize=patch->size()>=1024U*1024U;
    if(memoize)if(auto cached=find_gpu_pixel_cache("patch",inputs,key))return cached;
    auto result=std::make_shared<PixelPatch>(std::move(base),std::move(patch),offset,stride,row_bytes,rows);
    if(memoize)store_gpu_pixel_cache("patch",inputs,key,result);
    return result;
}
}
