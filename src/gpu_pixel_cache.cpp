#include "bgiplayer/gpu_pixel_cache.hpp"
#include <cstdlib>
#include <cstring>
#include <list>
#include <iterator>
#include <string>

namespace bgiplayer {
namespace {
constexpr size_t maximum_bytes=64U*1024U*1024U,maximum_entries=32;
struct Input { std::weak_ptr<GpuPixelStorage> owner;size_t bytes; };
struct Entry {
    std::string tag;
    std::vector<Input> inputs;
    std::vector<uint64_t> arguments;
    std::shared_ptr<GpuPixelStorage> result;
    size_t bytes;
};
struct Cache {
    std::list<Entry> entries;
    size_t bytes=0;
    uint64_t hits=0,misses=0;
    void erase(std::list<Entry>::iterator at) {bytes-=at->bytes;entries.erase(at);}
    void prune() {
        // Erasing a result may expire an earlier entry's weak input. Iterate
        // to a fixed point; at most32 entries can be removed in one cleanup.
        bool removed;
        do {
            removed=false;
            for(auto at=entries.begin();at!=entries.end();) {
                bool expired=false;
                for(const auto& input:at->inputs)if(input.owner.expired()){expired=true;break;}
                if(expired){auto old=at++;erase(old);removed=true;}else ++at;
            }
        } while(removed);
    }
};
Cache& cache(){static thread_local Cache value;return value;}
bool matches(const Entry& entry,std::string_view tag,
        std::span<const std::shared_ptr<GpuPixelStorage>> inputs,std::span<const uint64_t> arguments) {
    if(entry.tag!=tag || entry.inputs.size()!=inputs.size() || entry.arguments.size()!=arguments.size() ||
            !std::equal(entry.arguments.begin(),entry.arguments.end(),arguments.begin()))return false;
    for(size_t n=0;n<inputs.size();++n) {
        const auto& saved=entry.inputs[n];const auto& input=inputs[n];
        const auto owner=saved.owner.lock();
        // Compare both the object and its control block. A stale address or
        // an unrelated aliasing shared_ptr never revives a previous key.
        if(!input || !owner || owner.get()!=input.get() || saved.bytes!=input->size() ||
                saved.owner.owner_before(input) || input.owner_before(saved.owner))return false;
    }
    return true;
}
}
bool gpu_pixel_cache_enabled() {
    const char* gpu=std::getenv("BGI_GPU"),*enabled=std::getenv("BGI_PIXEL_CACHE");
    return !(gpu && (std::strcmp(gpu,"0")==0 || std::strcmp(gpu,"1")==0)) &&
        !(enabled && std::strcmp(enabled,"0")==0);
}
void clear_gpu_pixel_cache(){cache()=Cache{};}
GpuPixelCacheStatistics gpu_pixel_cache_statistics() {
    auto& value=cache();value.prune();
    return {value.entries.size(),value.bytes,value.hits,value.misses};
}
std::shared_ptr<GpuPixelStorage> find_gpu_pixel_cache(std::string_view tag,
        std::span<const std::shared_ptr<GpuPixelStorage>> inputs,std::span<const uint64_t> arguments) {
    if(!gpu_pixel_cache_enabled()){clear_gpu_pixel_cache();return {};}
    auto& value=cache();value.prune();
    for(auto at=value.entries.begin();at!=value.entries.end();++at) {
        if(!matches(*at,tag,inputs,arguments))continue;
        auto result=at->result;value.entries.splice(value.entries.begin(),value.entries,at);
        ++value.hits;return result;
    }
    ++value.misses;return {};
}
void store_gpu_pixel_cache(std::string_view tag,
        std::span<const std::shared_ptr<GpuPixelStorage>> inputs,std::span<const uint64_t> arguments,
        std::shared_ptr<GpuPixelStorage> result) {
    if(!gpu_pixel_cache_enabled()){clear_gpu_pixel_cache();return;}
    if(!result || !result->size() || result->size()>maximum_bytes)return;
    for(const auto& input:inputs)if(!input || input==result)return;
    auto& value=cache();value.prune();
    for(auto at=value.entries.begin();at!=value.entries.end();) {
        if(matches(*at,tag,inputs,arguments)){auto old=at++;value.erase(old);}else ++at;
    }
    const size_t bytes=result->size();
    Entry entry{std::string(tag),{},{arguments.begin(),arguments.end()},std::move(result),bytes};
    entry.inputs.reserve(inputs.size());
    for(const auto& input:inputs)entry.inputs.push_back({input,input->size()});
    while(!value.entries.empty() && (value.entries.size()>=maximum_entries || value.bytes>maximum_bytes-bytes))
        value.erase(std::prev(value.entries.end()));
    value.entries.push_front(std::move(entry));value.bytes+=bytes;
    value.prune();
}
}
