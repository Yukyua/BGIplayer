#include "bgiplayer/resource_decode_memo.hpp"
#include "bgiplayer/dsc.hpp"
#include "bgiplayer/render_policy.hpp"
#include <algorithm>
#include <exception>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <list>
#include <optional>
namespace bgiplayer {
namespace {
constexpr size_t minimum_bytes=64U*1024U,maximum_bytes=64U*1024U*1024U,maximum_entries=32;
struct Entry {ResourceDecodeMemoKey key;std::shared_ptr<GpuPixelStorage> pixels;size_t bytes;};
struct Memo {std::list<Entry> entries;size_t bytes=0;uint64_t hits=0,misses=0,read_failures=0;};
Memo& memo(){static thread_local Memo value;return value;}
void discard(const ResourceDecodeMemoKey& key) {
    auto& m=memo();
    for(auto at=m.entries.begin();at!=m.entries.end();) {
        if(at->key==key){m.bytes-=at->bytes;at=m.entries.erase(at);}else ++at;
    }
}
}
bool resource_decode_memo_enabled() {
    const char* gpu=std::getenv("BGI_GPU"),*enabled=std::getenv("BGI_RESOURCE_CACHE");
    return !retain_cpu_pixel_copies() &&
        !(gpu && (std::strcmp(gpu,"0")==0 || std::strcmp(gpu,"1")==0)) &&
        !(enabled && std::strcmp(enabled,"0")==0);
}
void clear_resource_decode_memo(){memo()=Memo{};}
ResourceDecodeMemoStatistics resource_decode_memo_statistics() {
    const auto& m=memo();return {m.entries.size(),m.bytes,m.hits,m.misses,m.read_failures};
}
std::shared_ptr<GpuPixelStorage> find_resource_decode_memo(const ResourceDecodeMemoKey& key) {
    if(!resource_decode_memo_enabled()){clear_resource_decode_memo();return {};}
    auto& m=memo();
    for(auto at=m.entries.begin();at!=m.entries.end();++at)if(at->key==key) {
        ++m.hits;m.entries.splice(m.entries.begin(),m.entries,at);return m.entries.front().pixels;
    }
    ++m.misses;return {};
}
void store_resource_decode_memo(const ResourceDecodeMemoKey& key,std::shared_ptr<GpuPixelStorage> pixels) {
    if(!resource_decode_memo_enabled()){clear_resource_decode_memo();return;}
    if(!pixels || pixels->size()<minimum_bytes || pixels->size()>maximum_bytes ||
            pixels->resident_bytes()>maximum_bytes)return;
    const size_t bytes=std::max(pixels->size(),pixels->resident_bytes());
    discard(key);auto& m=memo();
    while(!m.entries.empty() && (m.entries.size()>=maximum_entries || m.bytes>maximum_bytes-bytes)) {
        m.bytes-=m.entries.back().bytes;m.entries.pop_back();
    }
    m.entries.push_front({key,std::move(pixels),bytes});m.bytes+=bytes;
}
std::vector<uint8_t> decode_archive_resource(std::span<const uint8_t> encoded) {
    std::optional<ResourceDecodeMemoKey> key;
    if(!resource_decode_memo_enabled())clear_resource_decode_memo();
    else {
        try {
            key=bitmap_resource_memo_key(encoded,false);
            if(auto cached=find_resource_decode_memo(*key)) {
                std::vector<uint8_t> output(cached->size());
                if(cached->read(output))return output;
                ++memo().read_failures;discard(*key);
            }
        } catch(const std::exception&) {
            // Hash/allocation/readback failure is an optional host optimization
            // failure. Let the unchanged decoder decide the payload's status.
            if(key)discard(*key);
            key.reset();
        }
    }
    auto output=dsc_decode(encoded); // Failed decodes are never memoized.
    if(key && output.size()>=minimum_bytes && output.size()<=maximum_bytes) {
        try {store_resource_decode_memo(*key,store_host_pixel_storage(output));}
        catch(const std::exception&) {} // Preserve the successful decoder result.
    }
    return output;
}
} // namespace bgiplayer
