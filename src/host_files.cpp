#include "bgiplayer/host_files.hpp"
#include "bgiplayer/common.hpp"

#include <algorithm>
#include <array>
#include <iterator>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __APPLE__
#include <sys/attr.h>
#endif
#endif

namespace bgiplayer::host_files {
std::string separators(std::string_view text) {
    std::string result(text);
    std::replace(result.begin(), result.end(), '\\', '/');
    return result;
}

std::filesystem::path resolve(
    std::string_view text, const std::filesystem::path& primary,
    std::string_view current, const std::map<std::string, std::string>& mounts) {
    std::string normalized = separators(text);
    const bool drive = normalized.size() >= 2 && normalized[1] == ':' &&
        ((normalized[0] >= 'a' && normalized[0] <= 'z') ||
         (normalized[0] >= 'A' && normalized[0] <= 'Z'));
    if (drive) {
        std::string prefix = normalized.substr(0, 2);
        if (prefix[0] >= 'A' && prefix[0] <= 'Z') prefix[0] = static_cast<char>(prefix[0] + ('a' - 'A'));
        std::size_t start = 2;
        while (start < normalized.size() && normalized[start] == '/') ++start;
        if (const auto mount = mounts.find(prefix); mount != mounts.end())
            return (std::filesystem::path(mount->second) /
                    normalized.substr(start)).lexically_normal();
#ifndef _WIN32
        // Preserve the previous native resolver's unmounted-drive mapping.
        normalized.erase(0, start);
#endif
    }
    std::filesystem::path path(normalized);
    if (path.is_relative()) {
        if (!current.empty()) {
            std::filesystem::path directory(separators(current));
            if (directory.is_relative() && !primary.empty()) directory = primary / directory;
            path = directory / path;
        } else if (!primary.empty()) path = primary / path;
    }
    return path.lexically_normal();
}

std::ifstream open_read(const std::filesystem::path& path, std::ios::openmode mode) {
    return std::ifstream(path, mode);
}
std::ofstream open_write(const std::filesystem::path& path, std::ios::openmode mode) {
    return std::ofstream(path, mode);
}
std::vector<uint8_t> read(const std::filesystem::path& path) {
    auto input = open_read(path);
    if (!input) throw Error("cannot open file: " + path.string());
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
bool write(const std::filesystem::path& path, std::span<const uint8_t> bytes) {
    auto output = open_write(path);
    if (!output) return false;
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    output.close();
    return output.good();
}
bool exists(const std::filesystem::path& path, std::error_code& error) {
    return std::filesystem::exists(path, error);
}
bool is_regular_file(const std::filesystem::path& path, std::error_code& error) {
    return std::filesystem::is_regular_file(path, error);
}
bool is_regular_file(const std::filesystem::path& path) {
    return std::filesystem::is_regular_file(path);
}
bool is_directory(const std::filesystem::path& path, std::error_code& error) {
    return std::filesystem::is_directory(path, error);
}
bool is_directory(const std::filesystem::path& path) {
    std::error_code error;
    return host_files::is_directory(path, error) && !error;
}
uintmax_t file_size(const std::filesystem::path& path, std::error_code& error) {
    return std::filesystem::file_size(path, error);
}
uintmax_t file_size(const std::filesystem::path& path) {
    return std::filesystem::file_size(path);
}
std::filesystem::file_status status(const std::filesystem::path& path,
                                   std::error_code& error) {
    return std::filesystem::status(path, error);
}
void permissions(const std::filesystem::path& path, std::filesystem::perms value,
                 std::filesystem::perm_options options, std::error_code& error) {
    std::filesystem::permissions(path, value, options, error);
}
bool create_directory(const std::filesystem::path& path, std::error_code& error) {
    return std::filesystem::create_directory(path, error);
}
bool remove(const std::filesystem::path& path, std::error_code& error) {
    return std::filesystem::remove(path, error);
}
void rename(const std::filesystem::path& from, const std::filesystem::path& to) {
    std::filesystem::rename(from, to);
}
void rename(const std::filesystem::path& from, const std::filesystem::path& to,
            std::error_code& error) {
    std::filesystem::rename(from, to, error);
}
bool copy_file(const std::filesystem::path& from, const std::filesystem::path& to,
               std::filesystem::copy_options options, std::error_code& error) {
    return std::filesystem::copy_file(from, to, options, error);
}
std::filesystem::space_info space(const std::filesystem::path& path,
                                 std::error_code& error) {
    return std::filesystem::space(path, error);
}
std::vector<Entry> entries(const std::filesystem::path& root, bool recursive,
                           std::error_code& error) {
    std::vector<Entry> result;
    const auto append = [&](const std::filesystem::directory_entry& entry) {
        std::error_code entry_error;
        const bool regular = entry.is_regular_file(entry_error);
        entry_error.clear();
        const bool directory = entry.is_directory(entry_error);
        result.push_back({entry.path(), regular, directory});
    };
    if (recursive) {
        for (std::filesystem::recursive_directory_iterator it(
                 root, std::filesystem::directory_options::skip_permission_denied, error), end;
             it != end; it.increment(error)) {
            if (error) { error.clear(); continue; }
            append(*it);
        }
    } else {
        for (std::filesystem::directory_iterator it(
                 root, std::filesystem::directory_options::skip_permission_denied, error), end;
             it != end; it.increment(error)) {
            if (error) { error.clear(); continue; }
            append(*it);
        }
    }
    return result;
}

bool read_times(const std::filesystem::path& path, Times& times) {
#ifdef _WIN32
    HANDLE file = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    FILETIME creation{}, access{}, write{};
    const bool success = GetFileTime(file, &creation, &access, &write) != 0;
    CloseHandle(file);
    if (!success) return false;
    const auto convert = [](FILETIME time) {
        const uint64_t ticks = (uint64_t(time.dwHighDateTime) << 32U) | time.dwLowDateTime;
        return TimePoint{static_cast<int64_t>(ticks / 10000000ULL) - 11644473600LL,
                         static_cast<int32_t>((ticks % 10000000ULL) * 100)};
    };
    times = {convert(creation), convert(access), convert(write)};
#else
    struct stat status{};
    if (stat(path.c_str(), &status) != 0) return false;
    const auto convert = [](timespec time) {
        return TimePoint{static_cast<int64_t>(time.tv_sec), static_cast<int32_t>(time.tv_nsec)};
    };
#ifdef __APPLE__
    times = {convert(status.st_birthtimespec), convert(status.st_atimespec), convert(status.st_mtimespec)};
#else
    times = {convert(status.st_ctim), convert(status.st_atim), convert(status.st_mtim)};
#endif
#endif
    return true;
}
bool write_times(const std::filesystem::path& path, const Times& times) {
#ifdef _WIN32
    const auto valid = [](TimePoint time) {
        if (time.nanoseconds < 0 || time.nanoseconds >= 1000000000 ||
            time.seconds < -11644473600LL || time.seconds > 1833029933770LL) return false;
        const uint64_t seconds = static_cast<uint64_t>(time.seconds + 11644473600LL);
        return seconds < UINT64_MAX / 10000000ULL ||
            (seconds == UINT64_MAX / 10000000ULL &&
             static_cast<uint64_t>(time.nanoseconds / 100) <= UINT64_MAX % 10000000ULL);
    };
    if (!valid(times.creation) || !valid(times.access) || !valid(times.write)) return false;
    const auto convert = [](TimePoint time) {
        const uint64_t ticks = static_cast<uint64_t>(time.seconds + 11644473600LL) *
            10000000ULL + static_cast<uint64_t>(time.nanoseconds / 100);
        return FILETIME{static_cast<DWORD>(ticks), static_cast<DWORD>(ticks >> 32U)};
    };
    HANDLE file = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    const auto creation = convert(times.creation), access = convert(times.access), write = convert(times.write);
    const bool success = SetFileTime(file, &creation, &access, &write) != 0;
    CloseHandle(file);
    return success;
#else
    const auto convert = [](TimePoint time) {
        return timespec{static_cast<time_t>(time.seconds), static_cast<long>(time.nanoseconds)};
    };
#ifdef __APPLE__
    auto creation = convert(times.creation);
    attrlist attributes{};
    attributes.bitmapcount = ATTR_BIT_MAP_COUNT;
    attributes.commonattr = ATTR_CMN_CRTIME;
    if (setattrlist(path.c_str(), &attributes, &creation, sizeof(creation), 0) != 0)
        return false;
#endif
    const std::array<timespec, 2> values{convert(times.access), convert(times.write)};
    return utimensat(AT_FDCWD, path.c_str(), values.data(), 0) == 0;
#endif
}
} // namespace bgiplayer::host_files
