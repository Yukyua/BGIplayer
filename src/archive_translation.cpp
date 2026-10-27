#include "bgiplayer/archive.hpp"
#include "bgiplayer/translation_patch.hpp"
#include "bgiplayer/common.hpp"
namespace bgiplayer {
void ArchiveSet::load_translation_patch(const std::filesystem::path& path) {
    // Validate the complete package before exposing a single replacement.
    for (const auto& archive:archives_) if(archive.translation_patch_)
        throw Error("a translation patch is already installed");
    auto patch=std::make_shared<TranslationPatch>(TranslationPatch::load(path));
    for(const auto& entry:patch->entries()) {
        std::size_t matches=0;
        for(const auto& candidate:archives_)
            if(normalize_resource_name(candidate.path().filename().string())==entry.archive)++matches;
        if(matches>1)throw Error("translation patch archive is ambiguous: "+entry.archive);
        const auto* archive=find_archive(entry.archive);
        if(!archive || normalize_resource_name(archive->path().filename().string())!=entry.archive)
            throw Error("translation patch archive is missing: "+entry.archive);
        const auto* original=archive->find(entry.resource);
        if(!original)throw Error("translation patch resource is missing: "+entry.archive+":"+entry.resource);
        const auto payload=archive->read(*original);
        if(payload.size()!=entry.original_size || TranslationPatch::sha256(payload)!=entry.original_digest)
            throw Error("translation patch original content mismatch: "+entry.archive+":"+entry.resource);
    }
    for(auto& archive:archives_) archive.translation_patch_=patch;
}
std::string ArchiveSet::translation_patch_identity() const {
    if(archives_.empty() || !archives_.front().translation_patch_)return {};
    const auto& digest=archives_.front().translation_patch_->package_digest();
    constexpr char hex[]="0123456789abcdef";
    std::string identity;identity.reserve(64);
    for(const uint8_t byte:digest){identity+=hex[byte>>4U];identity+=hex[byte&15U];}
    return identity;
}
}
