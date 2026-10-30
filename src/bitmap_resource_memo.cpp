#include "bgiplayer/bitmap_resource_memo.hpp"
#include "bgiplayer/render_policy.hpp"
#include "bgiplayer/translation_patch.hpp"
#include "bgiplayer/validation.hpp"
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <list>
#include <iostream>
#include <limits>
#include <sstream>
#if defined(__APPLE__)
#include <CommonCrypto/CommonDigest.h>
#endif
namespace bgiplayer {
namespace {
constexpr size_t maximum_entries=32,maximum_bytes=128U*1024U*1024U;
struct Entry { BitmapResourceMemoKey key;BitmapResourceMemoValue value;size_t bytes; };
struct Memo { std::list<Entry> entries;size_t bytes=0;uint64_t hits=0,misses=0; };
Memo& memo(){static thread_local Memo value;return value;}
}
bool bitmap_resource_memo_enabled() {
    const char* gpu=std::getenv("BGI_GPU");
    const char* enabled=std::getenv("BGI_BITMAP_CACHE");
    return !retain_cpu_pixel_copies() && !(gpu && (std::strcmp(gpu,"0")==0 || std::strcmp(gpu,"1")==0)) &&
        !(enabled && std::strcmp(enabled,"0")==0);
}
BitmapResourceMemoKey bitmap_resource_memo_key(std::span<const uint8_t> data,bool pe64) {
    std::array<uint8_t,32> digest;
#if defined(__APPLE__)
    // System crypto uses the host's SHA acceleration without a second copy
    // of compressed bytes. Keep the existing portable digest as a fallback.
    if(data.size()<=std::numeric_limits<CC_LONG>::max()) {
        CC_SHA256(data.data(),static_cast<CC_LONG>(data.size()),digest.data());
    } else {
        CC_SHA256_CTX context;
        CC_SHA256_Init(&context);
        for(size_t offset=0;offset<data.size();) {
            const size_t bytes=std::min(data.size()-offset,size_t(std::numeric_limits<CC_LONG>::max()));
            CC_SHA256_Update(&context,data.data()+offset,static_cast<CC_LONG>(bytes));
            offset+=bytes;
        }
        CC_SHA256_Final(digest.data(),&context);
    }
#else
    digest=TranslationPatch::sha256(data);
#endif
    return {digest,data.size(),pe64,content_check_level()==CheckLevel::Strict};
}
void clear_bitmap_resource_memo(){memo()=Memo{};}
BitmapResourceMemoStatistics bitmap_resource_memo_statistics(){const auto& m=memo();return {m.entries.size(),m.bytes,m.hits,m.misses};}
void trace_bitmap_resource_preparation(uint32_t id,size_t bytes,bool hit,bool success,
        uint64_t read_ns,uint64_t prepare_ns) {
    std::cerr<<"BITMAP_RESOURCE_COST id="<<id<<" encoded_bytes="<<bytes
        <<" memo_hit="<<hit<<" success="<<success<<" read_ns="<<read_ns
        <<" prepare_ns="<<prepare_ns<<'\n';
}
void trace_bitmap_resource_prepare_detail(uint32_t id,size_t bytes,bool hit,bool success,
        const std::array<uint64_t,7>& phases) {
    std::ostringstream line;
    line<<"BITMAP_PREPARE_DETAIL id="<<id<<" encoded_bytes="<<bytes
        <<" memo_hit="<<hit<<" success="<<success
        <<" hash_lookup_ns="<<phases[0]<<" decode_ns="<<phases[1]
        <<" layout_ns="<<phases[2]<<" mirror_ns="<<phases[3]
        <<" memo_store_ns="<<phases[4]<<" install_ns="<<phases[5]
        <<" other_ns="<<phases[6]<<'\n';
    std::cerr<<line.str();
}
std::optional<BitmapResourceMemoValue> find_bitmap_resource_memo(const BitmapResourceMemoKey& key) {
    if(!bitmap_resource_memo_enabled()){clear_bitmap_resource_memo();return {};}
    auto& m=memo();
    for(auto at=m.entries.begin();at!=m.entries.end();++at)if(at->key==key) {
        ++m.hits;m.entries.splice(m.entries.begin(),m.entries,at);return m.entries.front().value;
    }
    ++m.misses;return {};
}
void store_bitmap_resource_memo(const BitmapResourceMemoKey& key,BitmapResourceMemoValue& value) {
    if(!bitmap_resource_memo_enabled()){clear_bitmap_resource_memo();return;}
    // Avoid forcing tiny CPU bitmaps onto the GPU merely to keep a memo.
    // RGBA is normally a presentation view of pixels, so account it
    // conservatively as a separate allocation to keep the cap strict.
    if(value.pixels.size()<65536U || value.encoded.empty())return;
    size_t bytes=0;
    for(const PixelBuffer* buffer:{&value.encoded,&value.pixels,&value.rgba}) {
        if(buffer->size()>maximum_bytes-bytes)return;
        bytes+=buffer->size();
    }
    if(!value.pixels.offload() || !value.encoded.offload() || !value.rgba.offload())return;
    auto& m=memo();
    for(auto at=m.entries.begin();at!=m.entries.end();) {
        if(at->key==key){m.bytes-=at->bytes;at=m.entries.erase(at);}else ++at;
    }
    while(!m.entries.empty() && (m.entries.size()>=maximum_entries || m.bytes>maximum_bytes-bytes)) {
        m.bytes-=m.entries.back().bytes;m.entries.pop_back();
    }
    m.entries.push_front({key,value,bytes});m.bytes+=bytes;
}
} // namespace bgiplayer
