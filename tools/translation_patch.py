#!/usr/bin/env python3
"""Extract verified Enigma/CPK compiled-text resources into BGITRP01.

Never executes the supplied EXE. Original DSC resources are decoded using the
existing engine --dump-resource interface. Generated copyrighted text stays local.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import zlib

MAGIC = b"BurikoCompiledScriptVer1.00\0"
MAX_BYTES = 256 * 1024 * 1024


def embedded_scripts(data, metadata=None):
    from enigma_vfs import compiled_scripts
    return compiled_scripts(data, metadata)



def cpk_scripts(cpk, key, openssl="openssl"):
    """Observed SimpleVFS CPK v2; only complete compiled text scripts are emitted.

    Cinderella_cn.dll 100048B0/10004C80 and EAX<Camellia> RTTI establish the
    directory and encryption path. The original calls ProcessData, with no
    appended authentication tag; zlib checksum and decoded sizes are validated.
    """
    if len(key) < 40 or key[:4] != b"CPK\0" or len(cpk) < 140:
        raise ValueError("invalid CPK key/container header")
    count = struct.unpack_from("<I", key, 4)[0]
    if not 0 < count <= 4096 or len(key) != 40 + count * 72:
        raise ValueError("unsupported CPK key table size")
    header = bytes(value ^ key[8 + index % 32] for index, value in enumerate(cpk[:140]))
    if header[:8] != b"CPK\0\x02\0\0\0" or struct.unpack_from("<I", header, 136)[0] != count:
        raise ValueError("CPK header/key identity mismatch")
    start = 140 + count * 44
    if start > len(cpk):
        raise ValueError("truncated CPK directory")
    results, omitted, names = [], [], set()
    cursor = 0
    for index in range(count):
        secret = key[40 + index * 72:40 + (index + 1) * 72]
        row = bytes(value ^ secret[position % 32] for position, value in enumerate(cpk[140 + index * 44:140 + (index + 1) * 44]))
        resource = row[:32].split(b"\0", 1)[0].decode("ascii").lower()
        if not resource or resource in names:
            raise ValueError("duplicate/invalid CPK resource name")
        names.add(resource)
        size, packed_size, offset = struct.unpack_from("<III", row, 32)
        key_length, iv_length = struct.unpack_from("<II", secret, 32)
        if key_length != 16 or iv_length != 16:
            raise ValueError("unsupported Camellia key/IV length")
        if offset != cursor or not 0 < size <= 64 * 1024 * 1024 or start + offset + packed_size > len(cpk):
            raise ValueError("invalid CPK resource bounds")
        cursor += packed_size
        key_hex = secret[40:56].hex()
        mac = subprocess.run([openssl, "mac", "-macopt", "cipher:CAMELLIA-128-CBC",
                              "-macopt", "hexkey:" + key_hex, "CMAC"],
                             input=bytes(16) + secret[56:72], capture_output=True, check=True)
        nonce = bytes.fromhex(mac.stdout.decode("ascii").strip())
        if len(nonce) != 16:
            raise ValueError("invalid Camellia-CMAC output")
        decrypted = subprocess.run([openssl, "enc", "-camellia-128-ctr", "-d", "-K", key_hex,
                                    "-iv", nonce.hex()], input=cpk[start + offset:start + offset + packed_size],
                                   capture_output=True, check=True).stdout
        decoder = zlib.decompressobj()
        payload = decoder.decompress(decrypted, size + 1)
        if len(payload) != size or not decoder.eof or decoder.unused_data or decoder.unconsumed_tail:
            raise ValueError("CPK zlib payload/size mismatch: " + resource)
        if payload.startswith(MAGIC):
            results.append({"resource": resource, "payload": payload,
                            "container_offset": start + offset, "directory_index": index})
        else:
            omitted.append({"resource": resource, "decoded_size": size,
                            "reason": "non-CompiledScript resource (UI bitmap or native BP)"})
    if start + cursor != len(cpk):
        raise ValueError("CPK container has trailing data")
    if not results:
        raise ValueError("CPK contains no supported compiled text scripts")
    return results, omitted


def archive_index(game):
    resources = {}
    for path in sorted(game.rglob("*.arc")):
        with path.open("rb") as handle:
            header = handle.read(16)
            if header[:12] == b"BURIKO ARC20":
                size = 128
            elif header[:12] == b"PackFile    ":
                size = 32
            else:
                continue
            count = struct.unpack_from("<I", header, 12)[0]
            if count > 0x0FFFFF:
                raise ValueError("invalid archive entry count")
            table = handle.read(size * count)
            if len(table) != size * count:
                raise ValueError("truncated archive table")
            for index in range(count):
                row = table[index * size:(index + 1) * size]
                name = row[:96 if size == 128 else 16].split(b"\0", 1)[0].decode("ascii").lower()
                resources.setdefault(name, []).append(path.name)
    return resources


def write_patch(entries, output):
    if not entries or len(entries) > 4096:
        raise ValueError("invalid entry count")
    blob = bytearray(b"BGITRP01" + struct.pack("<I", len(entries)))
    keys = set()
    total = 0
    for entry in entries:
        archive = entry["archive"].lower().encode("ascii")
        resource = entry["resource"].lower().encode("ascii")
        if not archive or not resource or len(archive) > 1024 or len(resource) > 1024 or b"\0" in archive + resource or b"/" in archive or b"\\" in archive:
            raise ValueError("invalid resource key")
        key = (archive, resource)
        if key in keys:
            raise ValueError("duplicate resource key")
        keys.add(key)
        payload = entry["payload"]
        total += len(payload)
        if not payload or len(payload) > 64 * 1024 * 1024 or total > MAX_BYTES:
            raise ValueError("replacement payload exceeds limit")
        packed = zlib.compress(payload, 9)
        blob.extend(struct.pack("<HHI32sII32s", len(archive), len(resource),
                                entry["original_size"], bytes.fromhex(entry["original_sha256"]),
                                len(payload), len(packed), hashlib.sha256(payload).digest()))
        blob.extend(archive + resource + packed)
    if len(blob) > MAX_BYTES:
        raise ValueError("patch exceeds file size limit")
    output.write_bytes(blob)


def utf8_texts(payload):
    cursor = 0
    for part in payload.split(b"\0"):
        try:
            text = part.decode("utf-8")
        except UnicodeDecodeError:
            text = ""
        if text and any("\u3400" <= ch <= "\u9fff" for ch in text) and not any(ord(ch) < 32 for ch in text):
            yield cursor, text
        cursor += len(part) + 1


def extract(args):
    overrides = {}
    override_path = getattr(args, "text_overrides", None)
    if override_path:
        if not args.changed_text_encoding:
            raise ValueError("--text-overrides requires --changed-text-encoding")
        document = json.loads(override_path.read_text(encoding="utf-8"))
        if document.get("format") != "BGI-TEXT-ENCODING-OVERRIDES-1":
            raise ValueError("unsupported text override format")
        for item in document["strings"]:
            key = item["source_sha256"]
            if len(key) != 64 or len(bytes.fromhex(key)) != 32 or key in overrides or item["encoding"] not in ("utf-8", "cp932") or "\0" in item["text"]:
                raise ValueError("invalid/duplicate text override")
            item["text"].encode(item["encoding"])
            overrides[key] = item
    if args.command == "extract-cpk":
        data, key = args.cpk.read_bytes(), args.key.read_bytes()
        entries, omitted = cpk_scripts(data, key, args.openssl)
        source_metadata = {"source_cpk_sha256": hashlib.sha256(data).hexdigest(),
                           "source_key_sha256": hashlib.sha256(key).hexdigest(),
                           "extraction": "verified-cpk2-camellia-stream-compiled-scripts",
                           "omitted_non_script": omitted}
    else:
        data = args.exe.read_bytes()
        source_metadata = {"source_exe_sha256": hashlib.sha256(data).hexdigest()}
        entries = embedded_scripts(data, source_metadata)
    if override_path:
        source_metadata["text_overrides_sha256"] = hashlib.sha256(override_path.read_bytes()).hexdigest()
    index = archive_index(args.game)
    if args.out.exists():
        raise ValueError("output directory exists; choose a new directory")
    # Validate all names against originals before creating output.
    for entry in entries:
        candidates = index.get(entry["resource"], [])
        if len(candidates) != 1:
            raise ValueError("original resource missing or ambiguous: " + entry["resource"])
        entry["archive"] = candidates[0]
    args.out.mkdir(parents=True)
    manifest = {"format": "BGITRP01", **source_metadata, "entries": []}
    text_count = 0
    with tempfile.TemporaryDirectory(prefix="bgi-translation-") as temporary, (args.out / "texts.jsonl").open("w", encoding="utf-8") as texts:
        for entry in entries:
            original = Path(temporary) / "original.bin"
            subprocess.run([str(args.engine.resolve()), "--game", str(args.game.resolve()),
                            "--no-translation-patch", "--dump-resource", entry["archive"] + ":" + entry["resource"],
                            "--dump-resource-output", str(original)], check=True, stdout=subprocess.DEVNULL)
            source = original.read_bytes()
            if not source.startswith(MAGIC):
                raise ValueError("original is not a compiled script: " + entry["resource"])
            if getattr(args, "changed_text_encoding", None):
                from compiled_text_patch import normalize_changed_strings
                entry["payload"], entry["text_conversion"] = normalize_changed_strings(
                    source, entry["payload"], args.changed_text_encoding, args.text_errors, overrides)
                invalid = entry["text_conversion"]["invalid_text_operands"]
                if invalid:
                    offsets = ",".join(hex(item["instruction_offset"]) for item in invalid)
                    print(f"warning: TRANSLATION_TEXT_INVALID resource={entry['resource']} operands={offsets} action=replace", file=sys.stderr)
            entry["original_size"] = len(source)
            entry["original_sha256"] = hashlib.sha256(source).hexdigest()
            resource_file = entry["resource"] + ".bin"
            (args.out / resource_file).write_bytes(entry["payload"])
            record = {key: value for key, value in entry.items() if key != "payload"}
            record["replacement_file"] = resource_file
            record["replacement_sha256"] = hashlib.sha256(entry["payload"]).hexdigest()
            record["replacement_size"] = len(entry["payload"])
            manifest["entries"].append(record)
            for offset, text in utf8_texts(entry["payload"]):
                texts.write(json.dumps({"archive": entry["archive"], "resource": entry["resource"],
                                        "offset": offset, "text": text}, ensure_ascii=False) + "\n")
                text_count += 1
    manifest["utf8_cjk_string_count"] = text_count
    (args.out / "manifest.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    write_patch(entries, args.out / "translation.bgitp")
    print(f"Extracted {len(entries)} compiled resources, {text_count} UTF-8 CJK strings; patch {args.out / 'translation.bgitp'}")


def pack(args):
    manifest = json.loads(args.manifest.read_text(encoding="utf-8"))
    if manifest.get("format") != "BGITRP01":
        raise ValueError("unsupported manifest format")
    entries = []
    for record in manifest["entries"]:
        entry = dict(record)
        entry["payload"] = (args.manifest.parent / entry["replacement_file"]).read_bytes()
        entries.append(entry)
    write_patch(entries, args.output)
    print(f"Packed {len(entries)} resource replacements into {args.output}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    extraction = commands.add_parser("extract-enigma")
    extraction.add_argument("--game", type=Path, required=True)
    extraction.add_argument("--exe", type=Path, required=True)
    extraction.add_argument("--engine", type=Path, default=Path("build/bgiplayer"))
    extraction.add_argument("--out", type=Path, required=True)
    extraction.add_argument("--changed-text-encoding", choices=("gbk", "cp932"),
                            help="convert changed typed string operands to UTF-8, keeping unchanged original bytes")
    extraction.add_argument("--text-errors", choices=("strict", "replace"), default="strict",
                            help="invalid source text rejects by default; explicit replace records affected operands")
    extraction.add_argument("--text-overrides", type=Path,
                            help="explicit source-SHA256-bound text/encoding overrides, e.g. for legacy name templates")
    cpk = commands.add_parser("extract-cpk", help="CPK v2 compiled text scripts, requiring OpenSSL 3")
    cpk.add_argument("--game", type=Path, required=True)
    cpk.add_argument("--cpk", type=Path, required=True)
    cpk.add_argument("--key", type=Path, required=True)
    cpk.add_argument("--openssl", default="openssl")
    cpk.add_argument("--engine", type=Path, default=Path("build/bgiplayer"))
    cpk.add_argument("--out", type=Path, required=True)
    packaging = commands.add_parser("pack")
    packaging.add_argument("manifest", type=Path)
    packaging.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        (extract if args.command.startswith("extract-") else pack)(args)
    except subprocess.CalledProcessError as error:
        parser.exit(1, f"translation patch: external resource decoder failed (exit {error.returncode})\n")
    except (ValueError, OSError, struct.error, zlib.error) as error:
        parser.exit(1, "translation patch: " + str(error) + "\n")

if __name__ == "__main__":
    main()
