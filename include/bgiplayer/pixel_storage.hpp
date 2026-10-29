#pragma once
#include <cstddef>
#include <algorithm>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>
#include <string_view>
namespace bgiplayer {
// Immutable host GPU allocation. It owns no decoded CPU pixel mirror.
class GpuPixelStorage {
public:
    virtual ~GpuPixelStorage()=default;
    [[nodiscard]] virtual size_t size() const=0;
    virtual const void* native_handle(std::string_view) const { return nullptr; }
    [[nodiscard]] virtual size_t resident_bytes() const { return size(); }
    virtual bool read(std::span<uint8_t> destination) const=0;
    virtual bool fast_ranges() const { return false; }
    virtual bool read_range(size_t offset,std::span<uint8_t> out) const {
        if(offset>size() || out.size()>size()-offset)return false;
        std::vector<uint8_t> bytes(size());
        if(!read(bytes))return false;
        std::copy_n(bytes.data()+offset,out.size(),out.data());return true;
    }
    virtual std::shared_ptr<GpuPixelStorage> concrete_storage() const { return {}; }
};
// Immutable presentation view of native GPU bytes; it owns no second pixel
// allocation. Conversion is performed only if a host actually requests RGBA.
std::shared_ptr<GpuPixelStorage> bgra_presentation_storage(
    std::shared_ptr<GpuPixelStorage> source, size_t pixels, bool opaque);
std::shared_ptr<GpuPixelStorage> patched_pixel_storage(std::shared_ptr<GpuPixelStorage> base,
    std::shared_ptr<GpuPixelStorage> patch,size_t offset,size_t stride,size_t row_bytes,size_t rows);
std::shared_ptr<GpuPixelStorage> store_host_pixel_storage(std::span<const uint8_t> pixels);
std::vector<std::shared_ptr<GpuPixelStorage>> store_host_pixel_storage_batch(std::span<const std::span<const uint8_t>> pixels);
}
