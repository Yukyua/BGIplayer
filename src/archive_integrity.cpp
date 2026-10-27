#include "bgiplayer/archive.hpp"
#include "bgiplayer/host_files.hpp"
namespace bgiplayer {
std::optional<ResourceIntegrityData> ArchiveSet::read_integrity_data(
    std::string_view archive,std::string_view name) const {
    ResourceIntegrityData data;
    const Archive* selected=nullptr;
    const ArchiveEntry* entry=nullptr;
    for(const auto* candidate:resolve_archives(archive)) {
        if(const auto* found=candidate->find(name)) {
            selected=candidate;entry=found;data.expected=entry->checksum;break;
        }
    }
    // Loose files take priority for the bytes, while the expected checksum
    // still comes from the named archive's resource table.
    const auto loose=host_files::resolve(name,game_root_);
    std::error_code error;
    const bool have_loose=host_files::is_regular_file(loose,error);
    if(!have_loose&&!entry)return std::nullopt;
    try {
        data.payload=have_loose?host_files::read(loose):selected->read_raw(*entry);
    }catch(const std::exception&){
        data.read_status=0xFFFFFFFFU;
    }
    return data;
}
}
