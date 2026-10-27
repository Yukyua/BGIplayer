#pragma once
#include <cstdint>
#include <filesystem>
#include <span>
#include <optional>
#include <string_view>
#include <string>
#include <vector>
namespace bgiplayer {
struct HostFontFileFace {
    std::string family;
    std::filesystem::path file;
    int64_t face_index = 0;
    uint32_t charset = 0;
    bool cjk_coverage = false;
    uint32_t weight = 400;
};
// Exact localized SFNT family names and Unicode cmap coverage.
std::vector<HostFontFileFace> inspect_font_file_faces(const std::filesystem::path& file);
struct HostFontReplacement {
    std::string requested;
    std::string family;
    std::filesystem::path source;
};
std::vector<HostFontReplacement> read_wine_font_replacements(const std::filesystem::path& registry);
std::vector<HostFontReplacement> host_font_replacements();
std::optional<HostFontFileFace> select_default_cjk_font(
    std::span<const HostFontFileFace> faces, std::string_view requested);
std::vector<std::filesystem::path> host_font_search_roots();
std::vector<HostFontFileFace> discover_host_font_faces(std::span<const std::filesystem::path> roots);
}
