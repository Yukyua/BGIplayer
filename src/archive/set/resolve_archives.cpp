// Complete definition: ArchiveSet::resolve_archives

namespace bgiplayer {
std::vector<const Archive*> ArchiveSet::resolve_archives(
    std::string_view archive) const {
    std::vector<const Archive*> result;
    const std::string key = archive_key(archive);
    const auto exact = by_archive_.find(key);
    if (exact != by_archive_.end()) {
        result.push_back(&archives_[exact->second]);
        return result;
    }

    const auto composite = composite_archives_.find(key);
    if (composite != composite_archives_.end()) {
        for (const std::string& component : composite->second) {
            const auto part = by_archive_.find(archive_key(component));
            if (part != by_archive_.end()) {
                result.push_back(&archives_[part->second]);
            }
        }
    }

    // data01xxx.arc and the other numbered aliases are registered at runtime
    // as composite archives by ipl._bp through Sys0:0x38. Archive-name lookup
    // itself is exact; 0x00407B50 compares the normalized names without a
    // wildcard expansion.
    return result;
}
}  // namespace bgiplayer
