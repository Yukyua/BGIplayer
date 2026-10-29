#include "bgiplayer/engine_profile.hpp"
#include "bgiplayer/engine_identity.hpp"
#include "bgiplayer/vm.hpp"

#include <string_view>

namespace bgiplayer {
namespace {
// op_unimplemented formats exactly
// "unimplemented BP opcode 0x<HEX> at 0x<offset> ...".
std::optional<uint8_t> reported_unimplemented_opcode(
    const std::string& error) {
    static constexpr std::string_view marker = "unimplemented BP opcode 0x";
    const std::size_t at = error.find(marker);
    if (at == std::string::npos) {
        return std::nullopt;
    }
    const std::size_t begin = at + marker.size();
    unsigned value = 0;
    bool digit = false;
    for (std::size_t index = begin;
         index < error.size() && index < begin + 8U; ++index) {
        const char character = error[index];
        unsigned nibble = 0;
        if (character >= '0' && character <= '9') {
            nibble = static_cast<unsigned>(character - '0');
        } else if (character >= 'A' && character <= 'F') {
            nibble = static_cast<unsigned>(character - 'A') + 10U;
        } else if (character >= 'a' && character <= 'f') {
            nibble = static_cast<unsigned>(character - 'a') + 10U;
        } else {
            break;
        }
        value = value * 16U + nibble;
        digit = true;
    }
    if (!digit) {
        return std::nullopt;
    }
    return static_cast<uint8_t>(value);
}
}  // namespace

std::optional<bool> probe_compact_64_from_bootstrap(
    const std::vector<uint8_t>& code, std::size_t limit) {
    if (code.empty()) {
        return std::nullopt;
    }
    VM probe(code);
    probe.set_compact_64_opcodes(false);
    std::size_t executed = 0;
    const VM::StepResult result = probe.run(limit, &executed);
    if (result == VM::StepResult::Fault) {
        const auto opcode = reported_unimplemented_opcode(probe.last_error());
        if (opcode && VM::is_compact_64_opcode(*opcode)) {
            return true;
        }
        return std::nullopt;
    }
    if (result == VM::StepResult::Yield && executed >= limit) {
        // The whole probed prefix executed under the legacy tables.
        return false;
    }
    return std::nullopt;
}

EngineProfileDecision analyze_engine_profile(
    const std::filesystem::path& game,
    const std::vector<uint8_t>& bootstrap) {
    EngineProfileDecision decision;
    if (const auto wide = discover_engine_64bit(game)) {
        decision.compact_64 = *wide;
        decision.evidence = "executable";
        return decision;
    }
    if (const auto probed = probe_compact_64_from_bootstrap(bootstrap)) {
        decision.compact_64 = *probed;
        decision.evidence = "bootstrap";
        return decision;
    }
    decision.compact_64 = false;
    decision.evidence = "undetermined";
    return decision;
}
}
