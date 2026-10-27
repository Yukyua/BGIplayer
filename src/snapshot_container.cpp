#include "bgiplayer/snapshot_container.hpp"
#include "bgiplayer/common.hpp"

#include <array>
#include <cstdlib>
#include <istream>
#include <ostream>
#include <zlib.h>

namespace bgiplayer {
namespace {
constexpr std::string_view magic = "BGIPLAYER_VM_SNAPSHOT";
constexpr uint64_t metadata_limit = 64 * 1024;
constexpr std::size_t chunk_size = 64 * 1024;
// compressBound(1 GiB) fits within this limit, including incompressible input.
constexpr uint64_t stored_limit = snapshot_payload_limit + 1024 * 1024;

void write_raw(std::ostream& out, const void* bytes, std::size_t size) {
    if (!size) return;
    out.write(static_cast<const char*>(bytes), static_cast<std::streamsize>(size));
    if (!out) throw Error("cannot write VM snapshot");
}
void read_raw(std::istream& in, void* bytes, std::size_t size) {
    if (!size) return;
    in.read(static_cast<char*>(bytes), static_cast<std::streamsize>(size));
    if (!in) throw Error("truncated VM snapshot");
}
template<class T> void write_number(std::ostream& out, T value) {
    std::array<uint8_t, sizeof(T)> bytes{};
    for (std::size_t i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<uint8_t>(value >> (i * 8));
    write_raw(out, bytes.data(), bytes.size());
}
template<class T> T read_number(std::istream& in) {
    std::array<uint8_t, sizeof(T)> bytes{};
    read_raw(in, bytes.data(), bytes.size());
    T value = 0;
    for (std::size_t i = 0; i < bytes.size(); ++i) value |= static_cast<T>(bytes[i]) << (i * 8);
    return value;
}
void write_string(std::ostream& out, std::string_view text) {
    if (text.size() > metadata_limit) throw Error("VM snapshot metadata is too large");
    write_number<uint64_t>(out, text.size());
    write_raw(out, text.data(), text.size());
}
std::string read_string(std::istream& in) {
    const auto size = read_number<uint64_t>(in);
    if (size > metadata_limit) throw Error("invalid VM snapshot metadata length");
    std::string text(static_cast<std::size_t>(size), '\0');
    read_raw(in, text.data(), text.size());
    return text;
}
struct Deflater {
    z_stream stream{};
    Deflater() {
        if (deflateInit(&stream, Z_BEST_SPEED) != Z_OK) throw Error("cannot initialize VM snapshot compression");
    }
    ~Deflater() { deflateEnd(&stream); }
};
struct Inflater {
    z_stream stream{};
    Inflater() {
        if (inflateInit(&stream) != Z_OK) throw Error("cannot initialize VM snapshot decompression");
    }
    ~Inflater() { inflateEnd(&stream); }
};
void deflate_payload(std::ostream& out, std::span<const uint8_t> payload) {
    Deflater codec;
    std::array<uint8_t, chunk_size> buffer{};
    std::size_t cursor = 0;
    int status = Z_OK;
    while (status != Z_STREAM_END) {
        if (!codec.stream.avail_in && cursor < payload.size()) {
            const auto count = std::min(chunk_size, payload.size() - cursor);
            codec.stream.next_in = const_cast<Bytef*>(payload.data() + cursor);
            codec.stream.avail_in = static_cast<uInt>(count);
            cursor += count;
        }
        codec.stream.next_out = buffer.data();
        codec.stream.avail_out = static_cast<uInt>(buffer.size());
        status = deflate(&codec.stream, cursor == payload.size() ? Z_FINISH : Z_NO_FLUSH);
        if (status != Z_OK && status != Z_STREAM_END) throw Error("VM snapshot compression failed");
        write_raw(out, buffer.data(), buffer.size() - codec.stream.avail_out);
    }
}
void inflate_payload(std::istream& in, uint64_t stored, std::vector<uint8_t>& payload) {
    Inflater codec;
    std::array<uint8_t, chunk_size> buffer{};
    uint8_t overflow_byte = 0;
    std::size_t cursor = 0;
    uint64_t remaining = stored;
    while (true) {
        if (!codec.stream.avail_in && remaining) {
            const auto count = static_cast<std::size_t>(std::min<uint64_t>(buffer.size(), remaining));
            read_raw(in, buffer.data(), count);
            remaining -= count;
            codec.stream.next_in = buffer.data();
            codec.stream.avail_in = static_cast<uInt>(count);
        }
        const bool full = cursor == payload.size();
        const auto capacity = full ? std::size_t{1} : std::min(chunk_size, payload.size() - cursor);
        codec.stream.next_out = full ? &overflow_byte : payload.data() + cursor;
        codec.stream.avail_out = static_cast<uInt>(capacity);
        const auto before_input = codec.stream.avail_in;
        const auto status = inflate(&codec.stream, Z_NO_FLUSH);
        const auto produced = capacity - codec.stream.avail_out;
        if (full && produced) throw Error("VM snapshot decompressed length exceeds declaration");
        cursor += produced;
        if (status == Z_STREAM_END) {
            if (remaining || codec.stream.avail_in) throw Error("trailing VM snapshot compressed data");
            if (cursor != payload.size()) throw Error("VM snapshot decompressed length mismatch");
            return;
        }
        if (status != Z_OK || (!produced && before_input == codec.stream.avail_in))
            throw Error("invalid or truncated VM snapshot compressed data");
    }
}
} // namespace

bool snapshot_compression_enabled() {
    const char* value = std::getenv("BGI_SNAPSHOT_COMPRESSION");
    return !value || std::string_view(value) != "0";
}

void write_snapshot_container(std::ostream& out, std::span<const uint8_t> payload,
                              std::string_view schema, std::string_view game_identity,
                              uint64_t timestamp, uint64_t checksum, bool compress) {
    if (payload.size() > snapshot_payload_limit) throw Error("snapshot size limit exceeded");
    write_string(out, magic);
    write_number<uint32_t>(out, compress ? 2 : 1);
    write_string(out, schema);
    write_string(out, game_identity);
    write_number<uint64_t>(out, timestamp);
    write_number<uint64_t>(out, payload.size());
    write_number<uint64_t>(out, checksum);
    if (!compress) { write_raw(out, payload.data(), payload.size()); return; }
    write_number<uint32_t>(out, 1); // RFC 1950 zlib wrapper around DEFLATE.
    const auto length_position = out.tellp();
    write_number<uint64_t>(out, 0);
    const auto payload_position = out.tellp();
    if (length_position < 0 || payload_position < 0) throw Error("VM snapshot output is not seekable");
    deflate_payload(out, payload);
    const auto end = out.tellp();
    if (end < payload_position) throw Error("invalid VM snapshot output position");
    const auto stored = static_cast<uint64_t>(end - payload_position);
    if (stored > stored_limit) throw Error("compressed VM snapshot is too large");
    out.seekp(length_position);
    write_number<uint64_t>(out, stored);
    out.seekp(end);
    if (!out) throw Error("cannot finish VM snapshot compression header");
}

SnapshotContainer read_snapshot_container(std::istream& in, std::string_view expected_schema) {
    in.seekg(0, std::ios::end);
    const auto end = in.tellg();
    if (end < 0 || static_cast<uint64_t>(end) > stored_limit + metadata_limit * 3 + 128)
        throw Error("VM snapshot is too large");
    in.seekg(0);
    if (read_string(in) != magic) throw Error("incompatible VM snapshot magic");
    const auto version = read_number<uint32_t>(in);
    if (version != 1 && version != 2) throw Error("incompatible VM snapshot version");
    SnapshotContainer result;
    result.schema = read_string(in);
    if (result.schema != expected_schema) throw Error("incompatible VM snapshot schema");
    result.game_identity = read_string(in);
    result.timestamp = read_number<uint64_t>(in);
    const auto length = read_number<uint64_t>(in);
    result.checksum = read_number<uint64_t>(in);
    uint64_t stored = length;
    if (version == 2) {
        if (read_number<uint32_t>(in) != 1) throw Error("unsupported VM snapshot compression codec");
        stored = read_number<uint64_t>(in);
    }
    if (length > snapshot_payload_limit || stored > stored_limit)
        throw Error("snapshot size limit exceeded");
    const auto start = in.tellg();
    if (start < 0 || start > end || stored != static_cast<uint64_t>(end - start))
        throw Error("VM snapshot length mismatch");
    result.payload.resize(static_cast<std::size_t>(length));
    if (version == 1) read_raw(in, result.payload.data(), result.payload.size());
    else inflate_payload(in, stored, result.payload);
    return result;
}
} // namespace bgiplayer
