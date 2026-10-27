#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace bgiplayer::host_files {

// Engine-visible paths are Windows paths. Host paths passed to I/O below
// are already resolved; archive lookup order and VM status codes live above
// this boundary. No operation changes the process working directory.
std::string separators(std::string_view text);
std::filesystem::path resolve(
    std::string_view text, const std::filesystem::path& primary = {},
    std::string_view current = {},
    const std::map<std::string, std::string>& mounts = {});

std::ifstream open_read(const std::filesystem::path& path, std::ios::openmode mode = std::ios::binary);
std::ofstream open_write(const std::filesystem::path& path, std::ios::openmode mode = std::ios::binary | std::ios::trunc);
std::vector<uint8_t> read(const std::filesystem::path& path);
bool write(const std::filesystem::path& path, std::span<const uint8_t> bytes);
bool exists(const std::filesystem::path& path, std::error_code& error);
bool is_regular_file(const std::filesystem::path& path, std::error_code& error);
bool is_regular_file(const std::filesystem::path& path);
bool is_directory(const std::filesystem::path& path, std::error_code& error);
bool is_directory(const std::filesystem::path& path);
uintmax_t file_size(const std::filesystem::path& path, std::error_code& error);
uintmax_t file_size(const std::filesystem::path& path);
std::filesystem::file_status status(const std::filesystem::path& path,
                                   std::error_code& error);
void permissions(const std::filesystem::path& path, std::filesystem::perms value,
                 std::filesystem::perm_options options, std::error_code& error);
bool create_directory(const std::filesystem::path& path, std::error_code& error);
bool remove(const std::filesystem::path& path, std::error_code& error);
void rename(const std::filesystem::path& from, const std::filesystem::path& to);
void rename(const std::filesystem::path& from, const std::filesystem::path& to,
            std::error_code& error);
bool copy_file(const std::filesystem::path& from, const std::filesystem::path& to,
               std::filesystem::copy_options options, std::error_code& error);
std::filesystem::space_info space(const std::filesystem::path& path,
                                 std::error_code& error);

struct Entry {
    std::filesystem::path path;
    bool regular = false;
    bool directory = false;
};
std::vector<Entry> entries(const std::filesystem::path& root, bool recursive,
                           std::error_code& error);

struct TimePoint { int64_t seconds = 0; int32_t nanoseconds = 0; };
struct Times { TimePoint creation, access, write; };
bool read_times(const std::filesystem::path& path, Times& times);
bool write_times(const std::filesystem::path& path, const Times& times);

} // namespace bgiplayer::host_files
