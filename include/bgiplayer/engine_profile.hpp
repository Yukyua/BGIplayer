#pragma once
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>
namespace bgiplayer {
// Result of the automatic engine-generation analysis performed before the
// runtime VM is created.
struct EngineProfileDecision {
    bool compact_64 = false;
    // "executable" (BGI PE machine type), "bootstrap" (the startup script
    // executed a 64-bit-only opcode) or "undetermined" (legacy default).
    std::string evidence = "undetermined";

};

// Executes the startup code under the legacy profile for up to `limit`
// dispatches. A fault that reports a 64-bit-only opcode is direct evidence
// for the compact generation (the legacy tables leave those slots empty);
// a prefix that runs cleanly keeps the legacy profile.
std::optional<bool> probe_compact_64_from_bootstrap(
    const std::vector<uint8_t>& code, std::size_t limit = 64U);

EngineProfileDecision analyze_engine_profile(
    const std::filesystem::path& game,
    const std::vector<uint8_t>& bootstrap);
}
