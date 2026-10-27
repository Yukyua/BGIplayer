#include "bgiplayer/decode_warnings.hpp"

#include <algorithm>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <set>
#include <sstream>
#include <string>

namespace bgiplayer {
void warn_decode_failure(std::string_view decoder, std::string_view reason,
                         std::span<const uint8_t> encoded, std::string_view resource) {
    // Decoder entry points are also used without a VM (CLI exports/audio).
    // Keep one thread-safe, process-local history outside serialized state.
    static std::mutex mutex;
    static std::set<std::string> reported;
    uint64_t fingerprint = 14695981039346656037ULL;
    for (const auto byte : encoded) fingerprint = (fingerprint ^ byte) * 1099511628211ULL;
    const std::string name(resource.substr(resource.find_last_of("/\\") + 1U));
    std::ostringstream key;
    key << decoder << '|' << reason << '|' << name << '|' << encoded.size() << '|' << fingerprint;
    const std::lock_guard guard(mutex);
    if (!reported.insert(key.str()).second) return;
    std::ostringstream out;
    out << "warning: DECODE_FAILED decoder=" << decoder << " reason=" << std::quoted(std::string(reason))
        << " resource=" << std::quoted(name) << " bytes=" << encoded.size()
        << " fingerprint=0x" << std::hex << fingerprint;
    const auto word = [&](size_t offset) {
        return uint32_t(encoded[offset]) | uint32_t(encoded[offset+1]) << 8U;
    };
    const auto dword = [&](size_t offset) { return word(offset) | word(offset+2) << 16U; };
    const auto magic = [&](std::string_view value) {
        return encoded.size() >= value.size() &&
            std::equal(value.begin(), value.end(), encoded.begin());
    };
    out << std::dec;
    if (magic("CompressedBG_MT") && encoded.size() >= 32U) {
        out << " format=CompressedBG_MT dimensions=" << dword(16) << 'x' << dword(20)
            << " stripe_rows=" << dword(24) << " bits=" << dword(28);
    } else if (magic("CompressedBG___") && encoded.size() >= 48U) {
        out << " format=CompressedBG version=" << word(46) << " dimensions=" << word(16)
            << 'x' << word(18) << " bits=" << word(20) << " selector=" << word(24);
    } else if (decoder == "bitmap" && encoded.size() >= 16U) {
        out << " format=raw-bitmap dimensions=" << word(0) << 'x' << word(2)
            << " bits=" << word(4) << " compression=" << word(6);
    }
    out << " header=" << std::hex << std::setfill('0');
    for (const auto byte : encoded.first(std::min<size_t>(48, encoded.size())))
        out << std::setw(2) << unsigned(byte);
    out << "; decode failed (diagnostic only)\n";
    std::cerr << out.str();
}
} // namespace bgiplayer
