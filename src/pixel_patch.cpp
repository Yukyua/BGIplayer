#include "bgiplayer/pixel_storage.hpp"
#include <mutex>
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
    std::shared_ptr<GpuPixelStorage> concrete_storage() const override {
        {std::lock_guard lock(mutex_);if(concrete_)return concrete_;}
        std::vector<uint8_t> bytes(bytes_);
        if(!read(bytes))return {};
        auto result=store_host_pixel_storage(bytes);
        if(!result)return {};
        std::lock_guard lock(mutex_);
        if(!concrete_){concrete_=std::move(result);base_.reset();patch_.reset();}
        return concrete_;
    }
};
}
std::shared_ptr<GpuPixelStorage> patched_pixel_storage(std::shared_ptr<GpuPixelStorage> base,
    std::shared_ptr<GpuPixelStorage> patch,size_t offset,size_t stride,size_t row_bytes,size_t rows) {
    if(!base || !patch || !base->fast_ranges() || !patch->fast_ranges() || !rows ||
        !row_bytes || rows>SIZE_MAX/row_bytes || stride<row_bytes || patch->size()!=row_bytes*rows || offset>base->size() ||
        (rows-1)>(base->size()-offset)/stride || row_bytes>base->size()-offset-(rows-1)*stride)return {};
    return std::make_shared<PixelPatch>(std::move(base),std::move(patch),offset,stride,row_bytes,rows);
}
}
