#pragma once

#include "bgiplayer/resource_integrity.hpp"
#include <cstdint>
#include <filesystem>
#include <optional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace bgiplayer {
class TranslationPatch;

struct ArchiveEntry {
    std::string name;
    uint32_t offset = 0;
    uint32_t size = 0;
    std::array<uint8_t,8> checksum{};
};

enum class ArchiveFormat { PackFile, BurikoArc20 };

class Archive {
public:
    static Archive open(const std::filesystem::path& path);

    [[nodiscard]] const std::filesystem::path& path() const { return path_; }
    [[nodiscard]] ArchiveFormat format() const { return format_; }
    [[nodiscard]] const std::vector<ArchiveEntry>& entries() const { return entries_; }
    [[nodiscard]] const ArchiveEntry* find(std::string_view name) const;
    [[nodiscard]] bool contains(std::string_view name) const {
        return find(name) != nullptr;
    }

    std::vector<uint8_t> read_raw(const ArchiveEntry& entry) const;
    std::vector<uint8_t> read(const ArchiveEntry& entry) const;
    std::vector<uint8_t> read(std::string_view name) const;

private:
    friend class ArchiveSet;
    std::shared_ptr<const TranslationPatch> translation_patch_;
    Archive(std::filesystem::path path, ArchiveFormat format,
            uint64_t data_start, std::vector<ArchiveEntry> entries);

    std::filesystem::path path_;
    ArchiveFormat format_;
    uint64_t data_start_;
    std::vector<ArchiveEntry> entries_;
    std::unordered_map<std::string, std::size_t> by_name_;
};

class ArchiveSet {
public:
    explicit ArchiveSet(std::filesystem::path game_root);

    void load_translation_patch(const std::filesystem::path& path);
    std::string translation_patch_identity() const;
    [[nodiscard]] const std::vector<Archive>& archives() const { return archives_; }
    [[nodiscard]] const Archive* find_archive(std::string_view name) const;
    [[nodiscard]] bool exists(std::string_view archive, std::string_view name) const;
    [[nodiscard]] uint32_t file_size(std::string_view archive,
                                     std::string_view name) const;
    std::vector<uint8_t> read(std::string_view archive, std::string_view name) const;
    std::optional<ResourceIntegrityData> read_integrity_data(
        std::string_view archive, std::string_view name) const;
    void register_composite_archive(std::string_view name,
                                    const std::vector<std::string>& components);
    std::vector<std::string> enumerate_files(std::string_view pattern,
                                             bool recursive,
                                             std::size_t limit = 0) const;

private:
    [[nodiscard]] std::vector<const Archive*> resolve_archives(
        std::string_view archive) const;

    std::filesystem::path game_root_;
    std::vector<Archive> archives_;
    std::unordered_map<std::string, std::size_t> by_archive_;
    std::unordered_map<std::string, std::vector<std::string>>
        composite_archives_;
};

}  // namespace bgiplayer
