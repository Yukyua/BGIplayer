#pragma once
#include "bgiplayer/debug.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace bgiplayer {

inline uint64_t raw_monotonic_ms() {
    using Clock = std::chrono::steady_clock;
    static const Clock::time_point epoch = Clock::now();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            Clock::now() - epoch)
            .count());
}

// Debug restore rebases engine time without counting time spent between runs.
// The zero offset preserves ordinary runtime behavior.
inline std::atomic<int64_t> snapshot_clock_offset_ms{0};
inline uint64_t monotonic_ms() {
    return static_cast<uint64_t>(static_cast<int64_t>(raw_monotonic_ms()) +
        snapshot_clock_offset_ms.load(std::memory_order_relaxed));
}
inline void restore_snapshot_clock(uint64_t saved_ms) {
    snapshot_clock_offset_ms.store(static_cast<int64_t>(saved_ms) -
        static_cast<int64_t>(raw_monotonic_ms()), std::memory_order_relaxed);
}

class Error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

inline std::string ascii_lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

inline std::string normalize_resource_name(std::string value) {
    std::replace(value.begin(), value.end(), '\\', '/');
    return ascii_lower(std::move(value));
}

inline bool matches_windows_pattern(std::string_view pattern,
                                    std::string_view value) {
    const std::string lowered_pattern = ascii_lower(std::string(pattern));
    const std::string lowered_value = ascii_lower(std::string(value));
    std::size_t pattern_index = 0;
    std::size_t value_index = 0;
    std::size_t star_index = std::string::npos;
    std::size_t star_value = 0;
    while (value_index < lowered_value.size()) {
        if (pattern_index < lowered_pattern.size() &&
            (lowered_pattern[pattern_index] == '?' ||
             lowered_pattern[pattern_index] == lowered_value[value_index])) {
            ++pattern_index;
            ++value_index;
        } else if (pattern_index < lowered_pattern.size() &&
                   lowered_pattern[pattern_index] == '*') {
            star_index = pattern_index++;
            star_value = value_index;
        } else if (star_index != std::string::npos) {
            pattern_index = star_index + 1;
            value_index = ++star_value;
        } else {
            return false;
        }
    }
    while (pattern_index < lowered_pattern.size() &&
           lowered_pattern[pattern_index] == '*') {
        ++pattern_index;
    }
    return pattern_index == lowered_pattern.size();
}

inline uint16_t read_u16(std::span<const uint8_t> data, std::size_t offset) {
    if (offset + 2 > data.size()) {
        throw Error("u16 read outside buffer");
    }
    return static_cast<uint16_t>(
        static_cast<uint16_t>(data[offset]) |
        static_cast<uint16_t>(static_cast<uint16_t>(data[offset + 1]) << 8U));
}

inline uint32_t read_u32(std::span<const uint8_t> data, std::size_t offset) {
    if (offset + 4 > data.size()) {
        throw Error("u32 read outside buffer");
    }
    return static_cast<uint32_t>(data[offset]) |
           (static_cast<uint32_t>(data[offset + 1]) << 8U) |
           (static_cast<uint32_t>(data[offset + 2]) << 16U) |
           (static_cast<uint32_t>(data[offset + 3]) << 24U);
}

inline std::string fixed_ascii(std::span<const uint8_t> data, std::size_t offset,
                               std::size_t size) {
    if (offset + size > data.size()) {
        throw Error("fixed string read outside buffer");
    }
    std::size_t length = 0;
    while (length < size && data[offset + length] != 0) {
        ++length;
    }
    return std::string(
        reinterpret_cast<const char*>(data.data() + offset), length);
}

std::vector<uint8_t> read_file(const std::filesystem::path& path);

}  // namespace bgiplayer
