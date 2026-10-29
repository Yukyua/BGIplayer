#include "bgiplayer/pixel_storage.hpp"
#include <limits>
namespace bgiplayer {
namespace {
class BgraPresentation final : public GpuPixelStorage {
    std::shared_ptr<GpuPixelStorage> source_;
    size_t bytes_;
    bool opaque_;
public:
    BgraPresentation(std::shared_ptr<GpuPixelStorage> source,size_t bytes,bool opaque)
        :source_(std::move(source)),bytes_(bytes),opaque_(opaque) {}
    size_t size() const override { return bytes_; }
    size_t resident_bytes() const override { return 0; } // source owns the allocation
    bool read(std::span<uint8_t> out) const override {
        if(out.size()!=bytes_)return false;
        std::vector<uint8_t> native(source_->size());
        if(!source_->read(native))return false;
        for(size_t at=0;at<bytes_;at+=4) {
            out[at]=native[at+2];out[at+1]=native[at+1];out[at+2]=native[at];
            out[at+3]=opaque_?255:native[at+3];
        }
        return true;
    }
};
}
std::shared_ptr<GpuPixelStorage> bgra_presentation_storage(
    std::shared_ptr<GpuPixelStorage> source,size_t pixels,bool opaque) {
    if(!source || pixels>std::numeric_limits<size_t>::max()/4 || pixels*4>source->size())return {};
    return std::make_shared<BgraPresentation>(std::move(source),pixels*4,opaque);
}
}
