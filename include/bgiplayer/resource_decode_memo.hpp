#pragma once
#include "bgiplayer/bitmap_resource_memo.hpp"
#include <memory>
#include <span>
#include <vector>
namespace bgiplayer {
// Host-only DSC/SDC decode memo. Keys contain the complete current raw
// payload's SHA256 and validation policy; archive/path names are not identities.
using ResourceDecodeMemoKey=BitmapResourceMemoKey;
struct ResourceDecodeMemoStatistics {size_t entries=0,bytes=0;uint64_t hits=0,misses=0,read_failures=0;};
bool resource_decode_memo_enabled();
void clear_resource_decode_memo();
ResourceDecodeMemoStatistics resource_decode_memo_statistics();
std::shared_ptr<GpuPixelStorage> find_resource_decode_memo(const ResourceDecodeMemoKey&);
void store_resource_decode_memo(const ResourceDecodeMemoKey&,std::shared_ptr<GpuPixelStorage>);
// Archive::read calls this only after translation lookup and read_raw. The
// returned vector is caller-owned temporary data, never a retained CPU mirror.
std::vector<uint8_t> decode_archive_resource(std::span<const uint8_t> encoded);
} // namespace bgiplayer
