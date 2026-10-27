#pragma once
#include <array>
#include <cstdint>
#include <span>
#include <vector>
namespace bgiplayer {
struct ResourceIntegrityData {
    std::vector<uint8_t> payload;
    std::array<uint8_t,8> expected{};
    uint32_t read_status = 0;
};
std::array<uint8_t,8> resource_checksum(std::span<const uint8_t> data);
uint32_t check_resource_integrity(const ResourceIntegrityData& data);
}
