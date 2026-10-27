#pragma once
#include <array>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bgiplayer {
std::optional<std::filesystem::path> discover_translation_patch(
    const std::filesystem::path& game,
    const std::optional<std::filesystem::path>& explicit_path = std::nullopt);

// Host resource overlay, not a VM opcode or a mutation of original archives.
class TranslationPatch {
public:
    using Digest = std::array<uint8_t, 32>;
    struct Entry {
        std::string archive;
        std::string resource;
        uint32_t original_size = 0;
        Digest original_digest{};
        std::vector<uint8_t> replacement;
    };
    static TranslationPatch load(const std::filesystem::path& path);
    static Digest sha256(std::span<const uint8_t> bytes);
    const Entry* find(std::string_view archive, std::string_view resource) const;
    const std::vector<Entry>& entries() const { return entries_; }
    const Digest& package_digest() const { return package_digest_; }
private:
    std::vector<Entry> entries_;
    Digest package_digest_{};
};
}
