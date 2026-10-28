#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>
namespace bgiplayer {
// Immutable host GPU allocation. It owns no decoded CPU pixel mirror.
class GpuPixelStorage {
public:
    virtual ~GpuPixelStorage()=default;
    [[nodiscard]] virtual size_t size() const=0;
    virtual bool read(std::span<uint8_t> destination) const=0;
};
std::shared_ptr<GpuPixelStorage> store_host_pixel_storage(std::span<const uint8_t> pixels);
std::vector<std::shared_ptr<GpuPixelStorage>> store_host_pixel_storage_batch(std::span<const std::span<const uint8_t>> pixels);
}
