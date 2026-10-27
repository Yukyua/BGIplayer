#include "bgiplayer/translation_patch.hpp"
#include "bgiplayer/common.hpp"
#include "bgiplayer/host_files.hpp"
#include <algorithm>
#include <bit>
#include <set>
#include <zlib.h>

namespace bgiplayer {
std::optional<std::filesystem::path> discover_translation_patch(
    const std::filesystem::path& game,
    const std::optional<std::filesystem::path>& explicit_path) {
    if(explicit_path)return explicit_path;
    const auto candidate=game/"translation_patch";
    std::error_code error;
    if(!host_files::exists(candidate,error)) {
        if(error)throw Error("cannot inspect translation_patch: "+error.message());
        return std::nullopt;
    }
    if(host_files::is_regular_file(candidate,error))return candidate;
    if(error)throw Error("cannot inspect translation_patch: "+error.message());
    if(!host_files::is_directory(candidate,error))
        throw Error("translation_patch must be a patch file or directory");
    std::vector<std::filesystem::path> matches;
    for(const auto& entry:host_files::entries(candidate,false,error))
        if(entry.regular && ascii_lower(entry.path.extension().string())==".bgitp")matches.push_back(entry.path);
    if(error)throw Error("cannot enumerate translation_patch: "+error.message());
    if(matches.size()!=1)
        throw Error("translation_patch directory must contain exactly one .bgitp file");
    return matches.front();
}

TranslationPatch::Digest TranslationPatch::sha256(std::span<const uint8_t> bytes) {
    static constexpr uint32_t constants[64] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
    uint32_t state[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    const auto block = [&](const uint8_t* p) {
        uint32_t w[64]{};
        for (unsigned i=0;i<16;++i) for (unsigned j=0;j<4;++j) w[i]=(w[i]<<8U)|p[i*4U+j];
        for (unsigned i=16;i<64;++i) {
            const auto a=w[i-15],b=w[i-2];
            w[i]=w[i-16]+(std::rotr(a,7)^std::rotr(a,18)^(a>>3U))+w[i-7]+(std::rotr(b,17)^std::rotr(b,19)^(b>>10U));
        }
        uint32_t a=state[0],b=state[1],c=state[2],d=state[3],e=state[4],f=state[5],g=state[6],h=state[7];
        for (unsigned i=0;i<64;++i) {
            const uint32_t t1=h+(std::rotr(e,6)^std::rotr(e,11)^std::rotr(e,25))+((e&f)^((~e)&g))+constants[i]+w[i];
            const uint32_t t2=(std::rotr(a,2)^std::rotr(a,13)^std::rotr(a,22))+((a&b)^(a&c)^(b&c));
            h=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;
        }
        state[0]+=a;state[1]+=b;state[2]+=c;state[3]+=d;state[4]+=e;state[5]+=f;state[6]+=g;state[7]+=h;
    };
    std::size_t cursor=0;
    for (;bytes.size()-cursor>=64;cursor+=64) block(bytes.data()+cursor);
    std::array<uint8_t,128> tail{};
    std::copy(bytes.begin()+static_cast<std::ptrdiff_t>(cursor),bytes.end(),tail.begin());
    tail[bytes.size()-cursor]=0x80;
    const std::size_t length=bytes.size()-cursor<56?64:128;
    const uint64_t bits=static_cast<uint64_t>(bytes.size())*8U;
    for (unsigned j=0;j<8;++j) tail[length-1U-j]=static_cast<uint8_t>(bits>>(j*8U));
    block(tail.data());if(length==128)block(tail.data()+64);
    Digest result{};
    for (unsigned i=0;i<8;++i)for(unsigned j=0;j<4;++j)result[i*4U+j]=static_cast<uint8_t>(state[i]>>(24U-j*8U));
    return result;
}

TranslationPatch TranslationPatch::load(const std::filesystem::path& path) {
    constexpr uint32_t maximum=256U*1024U*1024U;
    if(host_files::file_size(path)>maximum)throw Error("translation patch exceeds size limit");
    const auto bytes=host_files::read(path);
    if(bytes.size()<12 || std::string_view(reinterpret_cast<const char*>(bytes.data()),8)!="BGITRP01")
        throw Error("unsupported translation patch header");
    const uint32_t count=read_u32(bytes,8);
    if(count==0 || count>4096)throw Error("invalid translation patch entry count");
    std::size_t cursor=12,total=0;
    const auto take=[&](std::size_t size)->std::span<const uint8_t>{
        if(size>bytes.size()-cursor)throw Error("truncated translation patch");
        auto result=std::span<const uint8_t>(bytes).subspan(cursor,size);cursor+=size;return result;
    };
    TranslationPatch patch;
    std::set<std::pair<std::string,std::string>> keys;
    for(uint32_t i=0;i<count;++i){
        const auto header=take(80);
        const auto archive_length=read_u16(header,0),resource_length=read_u16(header,2);
        Entry entry;entry.original_size=read_u32(header,4);
        std::copy_n(header.begin()+8,32,entry.original_digest.begin());
        const uint32_t size=read_u32(header,40),packed_size=read_u32(header,44);
        Digest digest{};std::copy_n(header.begin()+48,32,digest.begin());
        if(archive_length==0||resource_length==0||archive_length>1024||resource_length>1024||size==0||size>64U*1024U*1024U||entry.original_size>64U*1024U*1024U||total+size>maximum)
            throw Error("invalid translation patch entry bounds");
        const auto archive=take(archive_length),resource=take(resource_length);
        entry.archive=normalize_resource_name(std::string(reinterpret_cast<const char*>(archive.data()),archive.size()));
        entry.resource=normalize_resource_name(std::string(reinterpret_cast<const char*>(resource.data()),resource.size()));
        const auto invalid_name=[](unsigned char c){return c<32U||c>=128U;};
        if(std::any_of(entry.archive.begin(),entry.archive.end(),invalid_name)||std::any_of(entry.resource.begin(),entry.resource.end(),invalid_name)||entry.archive.find('/')!=std::string::npos||!keys.emplace(entry.archive,entry.resource).second)
            throw Error("invalid or duplicate translation patch resource key");
        const auto packed=take(packed_size);entry.replacement.resize(size);
        uLongf output_size=size;uLong input_size=packed_size;
        const int status=uncompress2(entry.replacement.data(),&output_size,packed.data(),&input_size);
        if(status!=Z_OK||output_size!=size||input_size!=packed_size||sha256(entry.replacement)!=digest)
            throw Error("translation patch payload is corrupt: "+entry.archive+":"+entry.resource);
        total+=size;patch.entries_.push_back(std::move(entry));
    }
    if(cursor!=bytes.size())throw Error("translation patch has trailing data");
    patch.package_digest_=sha256(bytes);
    return patch;
}
const TranslationPatch::Entry* TranslationPatch::find(std::string_view archive,std::string_view resource)const {
    const auto a=normalize_resource_name(std::string(archive)),r=normalize_resource_name(std::string(resource));
    for(const auto& entry:entries_)if(entry.archive==a&&entry.resource==r)return &entry;
    return nullptr;
}
}
