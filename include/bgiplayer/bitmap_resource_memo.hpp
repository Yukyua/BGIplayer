#pragma once
#include "bgiplayer/pixel_buffer.hpp"
#include <array>
#include <cstdint>
#include <optional>
#include <span>
namespace bgiplayer {
// Host-only decode memo. No VM handle, token, archive name or mutable pixels
// are retained. A hit still passes through the bitmap manager's install path.
struct BitmapResourceMemoKey {
    std::array<uint8_t,32> digest{};
    size_t encoded_bytes=0;
    bool pe64=false, strict=false;
    bool operator==(const BitmapResourceMemoKey&) const = default;
};
struct BitmapResourceMemoValue {
    uint32_t width=0,height=0,format=0,bytes_per_pixel=0,stride=0;
    PixelBuffer encoded,pixels,rgba;
};
struct BitmapResourceMemoStatistics { size_t entries=0,bytes=0;uint64_t hits=0,misses=0; };
bool bitmap_resource_memo_enabled();
BitmapResourceMemoKey bitmap_resource_memo_key(std::span<const uint8_t>,bool pe64);
std::optional<BitmapResourceMemoValue> find_bitmap_resource_memo(const BitmapResourceMemoKey&);
// Offloads successful decoded bytes only; an unavailable GPU leaves the
// existing CPU loader result intact and does not populate the memo.
void store_bitmap_resource_memo(const BitmapResourceMemoKey&,BitmapResourceMemoValue&);
void clear_bitmap_resource_memo();
BitmapResourceMemoStatistics bitmap_resource_memo_statistics();
void trace_bitmap_resource_preparation(uint32_t id,size_t bytes,bool hit,bool success,
    uint64_t read_ns,uint64_t prepare_ns);
void trace_bitmap_resource_prepare_detail(uint32_t id,size_t bytes,bool hit,bool success,
    const std::array<uint64_t,7>& phases);
} // namespace bgiplayer
