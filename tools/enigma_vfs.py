"""Bounded offline reader for the two observed Enigma EVB layouts.

No EXE is executed. Directory and 64 KiB aPLib block evidence is retained in
ghproj/kanojostep_translation/. Unknown node/compression layouts are rejected.
"""
import struct

SCRIPT_MAGIC = b"BurikoCompiledScriptVer1.00\0"
MAX_FILE = 64 * 1024 * 1024
MAX_TOTAL = 1024 * 1024 * 1024


def aplib_decode(data, expected):
    if not 0 < expected <= 65536:
        raise ValueError("invalid EVB decoded block size")
    cursor = bits = tag = previous = last_match = 0
    output = bytearray()

    def byte():
        nonlocal cursor
        if cursor >= len(data):
            raise ValueError("truncated EVB aPLib stream")
        value = data[cursor]
        cursor += 1
        return value

    def bit():
        nonlocal bits, tag
        if bits == 0:
            tag, bits = byte(), 8
        value = tag >> 7
        tag = (tag << 1) & 255
        bits -= 1
        return value

    def gamma():
        value = 1
        while True:
            value = (value << 1) | bit()
            if value > MAX_FILE:
                raise ValueError("oversized EVB aPLib integer")
            if not bit():
                return value

    def literal(value):
        if len(output) >= expected:
            raise ValueError("EVB aPLib output exceeds block size")
        output.append(value)

    def copy(distance, length):
        if not 0 < distance <= len(output) or length > expected - len(output):
            raise ValueError("invalid EVB aPLib back-reference")
        for _ in range(length):
            output.append(output[-distance])

    literal(byte())
    while True:
        if not bit():
            literal(byte())
            last_match = 0
        elif not bit():
            distance = gamma()
            if not last_match and distance == 2:
                distance, length = previous, gamma()
            else:
                distance = ((distance - (2 if last_match else 3)) << 8) | byte()
                length = gamma() + (distance >= 32000) + (distance >= 1280) + 2 * (distance < 128)
                previous = distance
            copy(distance, length)
            last_match = 1
        elif not bit():
            value = byte()
            distance, length = value >> 1, 2 + (value & 1)
            if distance == 0:
                break
            copy(distance, length)
            previous, last_match = distance, 1
        else:
            distance = 0
            for _ in range(4):
                distance = (distance << 1) | bit()
            if distance:
                copy(distance, 1)
            else:
                literal(0)
            last_match = 0
    if cursor != len(data) or len(output) != expected:
        raise ValueError("EVB aPLib block size/termination mismatch")
    return bytes(output)


def evb_directory(blob):
    # The observed EVB header precedes the directory, within the first KiB.
    signature = bytes(blob[:1024]).find(b"EVB\0")
    if signature < 0 or signature + 8 > len(blob):
        raise ValueError("EVB header not found")
    header_size = struct.unpack_from("<I", blob, signature + 4)[0]
    if header_size != 64:
        raise ValueError("unsupported EVB header size")
    directory = signature + header_size
    if directory + 4 > len(blob):
        raise ValueError("truncated EVB directory header")
    size = struct.unpack_from("<I", blob, directory)[0]
    base = directory + 4 + size
    if size < 15 or base > len(blob):
        raise ValueError("invalid EVB directory bounds")
    cursor, nodes = directory + 4, 0
    entries, names = [], set()

    def take(length):
        nonlocal cursor
        if length > base - cursor:
            raise ValueError("truncated EVB node")
        result = blob[cursor:cursor + length]
        cursor += length
        return result

    reserved, roots = struct.unpack_from("<QI", take(12))
    if reserved != 0 or bytes(take(3)) != bytes(3) or not 0 < roots <= 4096:
        raise ValueError("unsupported EVB directory prefix")

    def node(depth):
        nonlocal nodes
        nodes += 1
        if depth > 32 or nodes > 8192:
            raise ValueError("EVB directory exceeds node/depth limit")
        position = cursor
        node_id, node_type, children = struct.unpack_from("<III", take(12))
        if children > 4096:
            raise ValueError("oversized EVB child count")
        raw = bytearray()
        while True:
            unit = bytes(take(2))
            if unit == b"\0\0":
                break
            raw.extend(unit)
            if len(raw) > 2048:
                raise ValueError("oversized EVB name")
        name = raw.decode("utf-16le")
        kind = struct.unpack("<H", take(2))[0]
        if kind == 3:
            if node_type != 1:
                raise ValueError("unsupported EVB folder node")
            take(28)
            for _ in range(children):
                node(depth + 1)
        elif kind == 2:
            if node_type != 2 or children != 0:
                raise ValueError("unsupported EVB file node")
            row = take(56)
            compression = row[0]
            decoded = struct.unpack_from("<Q", row, 1)[0]
            offset, stored = struct.unpack_from("<QQ", row, 40)
            key = name.lower()
            if not name or key in names or any(ord(ch) < 32 or ord(ch) >= 128 for ch in name) or "/" in name or "\\" in name:
                raise ValueError("invalid/duplicate EVB resource name")
            if compression not in (0, 3) or not 0 < decoded <= MAX_FILE or not 0 < stored <= MAX_FILE:
                raise ValueError("unsupported EVB compression/file size")
            names.add(key)
            entries.append({"resource": key, "directory_offset": position,
                            "offset": offset, "stored_size": stored,
                            "decoded_size": decoded, "compression": compression})
        else:
            raise ValueError("unsupported EVB node kind")

    for _ in range(roots):
        node(0)
    if cursor != base or not 0 < len(entries) <= 4096:
        raise ValueError("EVB directory size/count mismatch")
    cursor = total = 0
    for entry in sorted(entries, key=lambda item: item["offset"]):
        if entry["offset"] != cursor or base + cursor + entry["stored_size"] > len(blob):
            raise ValueError("noncontiguous/out-of-bounds EVB data")
        cursor += entry["stored_size"]
        total += entry["decoded_size"]
    if total > MAX_TOTAL:
        raise ValueError("EVB decoded storage exceeds limit")
    return base, entries


def decode_file(data, size, compression):
    if not 0 < size <= MAX_FILE or compression not in (0, 3):
        raise ValueError("unsupported EVB compression/file size")
    if compression == 0:
        if len(data) != size:
            raise ValueError("uncompressed EVB size mismatch")
        return bytes(data)
    count = (size + 65535) // 65536
    cursor = count * 12
    if cursor > len(data):
        raise ValueError("truncated EVB block table")
    output = bytearray()
    for index in range(count):
        offset, packed = struct.unpack_from("<QI", data, index * 12)
        if offset != cursor or packed == 0 or packed > len(data) - cursor:
            raise ValueError("invalid EVB block bounds")
        output.extend(aplib_decode(data[cursor:cursor + packed], min(65536, size - len(output))))
        cursor += packed
    if cursor != len(data) or len(output) != size:
        raise ValueError("EVB file size mismatch")
    return bytes(output)


def compiled_scripts(data, metadata=None):
    if len(data) < 64 or data[:2] != b"MZ":
        raise ValueError("not a PE executable")
    pe = struct.unpack_from("<I", data, 60)[0]
    if pe + 24 > len(data) or data[pe:pe + 4] != b"PE\0\0":
        raise ValueError("invalid PE signature")
    count = struct.unpack_from("<H", data, pe + 6)[0]
    table = pe + 24 + struct.unpack_from("<H", data, pe + 20)[0]
    if not 0 < count <= 96 or table + count * 40 > len(data):
        raise ValueError("invalid PE section table")
    results, omitted, containers = [], [], 0
    for index in range(count):
        section = table + index * 40
        if data[section:section + 8] != b".enigma1":
            continue
        containers += 1
        raw_size, raw_offset = struct.unpack_from("<II", data, section + 16)
        if raw_offset + raw_size > len(data):
            raise ValueError("truncated .enigma1 section")
        blob = memoryview(data)[raw_offset:raw_offset + raw_size]
        base, entries = evb_directory(blob)
        for entry in entries:
            start = base + entry["offset"]
            payload = decode_file(blob[start:start + entry["stored_size"]], entry["decoded_size"], entry["compression"])
            record = {"resource": entry["resource"], "exe_offset": raw_offset + start,
                      "directory_offset": raw_offset + entry["directory_offset"],
                      "stored_size": entry["stored_size"], "compression": entry["compression"]}
            if payload.startswith(SCRIPT_MAGIC):
                results.append({**record, "payload": payload})
            else:
                omitted.append({**record, "decoded_size": len(payload)})
    if containers != 1 or not results:
        raise ValueError("supported .enigma1 script container not found")
    if metadata is not None:
        metadata.update({"extraction": "verified-evb-directory-aplib-or-stored-compiled-scripts",
                         "container_file_count": len(results) + len(omitted),
                         "omitted_non_script": omitted})
    return results
