#pragma once

#include <cstdint>
#include <iosfwd>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bgiplayer {
// Transport only: the VM schema and game-save formats are unchanged.
inline constexpr uint64_t snapshot_payload_limit = 1024ULL * 1024 * 1024;
struct SnapshotContainer {
    std::string schema;
    std::string game_identity;
    uint64_t timestamp = 0;
    uint64_t checksum = 0;
    std::vector<uint8_t> payload;
};
bool snapshot_compression_enabled();
void write_snapshot_container(std::ostream& out, std::span<const uint8_t> payload,
                              std::string_view schema, std::string_view game_identity,
                              uint64_t timestamp, uint64_t checksum, bool compress);
SnapshotContainer read_snapshot_container(std::istream& in, std::string_view expected_schema);
} // namespace bgiplayer
