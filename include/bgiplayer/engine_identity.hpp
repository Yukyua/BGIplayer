#pragma once
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
namespace bgiplayer {
// Only the verified Sys0:E8 wrapper layouts of the two supported targets.
std::optional<std::string> engine_identifier_from_pe(std::span<const uint8_t> bytes);
std::optional<std::string> discover_engine_identifier(const std::filesystem::path& game);
// PE machine type: true for PE32+ x86-64 (the BURIKO GDB 3.00 generation),
// false for PE32 i386. std::nullopt for an unrecognized image.
std::optional<bool> engine_is_64bit_from_pe(std::span<const uint8_t> bytes);
std::optional<bool> discover_engine_64bit(const std::filesystem::path& game);
// Follow the original Grp1:66 table entry to its checked configure core.
std::optional<uint32_t> engine_effector_mode_limit_from_pe(std::span<const uint8_t> bytes);
std::optional<uint32_t> discover_effector_mode_limit(const std::filesystem::path& game);
}
