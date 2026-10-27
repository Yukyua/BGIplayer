#!/usr/bin/env python3
"""Bounded BGI compiled-script text inventory and append-only translation builder.

No translation service is contacted. Human/model translations are local JSONL.
Only explicit decoded string operands passed to the observed message operation
are offered as dialogue/speaker records; paths, debug labels and identifiers
remain unchanged. Unknown opcode layouts are rejected, never integer-scanned.
"""
import argparse
from collections import Counter
from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import translation_patch

MAGIC = translation_patch.MAGIC
# Observed opcodes of the has_t compiled-script resources, extended below by
# three original-script-verified KanojoStep handlers. Native operations
# have no inline operands; values are pushed separately. These are BS word
# opcodes, not the compact BP byte opcodes of the PE64 interpreter.
OBSERVED = set(int(word, 16) for word in """
0 1 2 3 8 9 a 10 11 16 17 18 19 1a 1b 1c 1d 1e 1f 20 21 22 23 2b 30 31 32 33 35 38 3a 3f 48 7f 81 a0 c0
 e0 e1 e2 e3 e4 e5 e6 e7 f0 f3 f4 f6 f7 f9 fb fe
100 107 108 109 10e 118 119 11d 11e 11f 120 12a 136 137 139 13a 13e 13f 140 143 14a 150 151 152 153 158 15d
164 166 169 16c 170 171 174 176 177 185 18a 191 194 195 19d 1a1 1a3 1ab 1e0 1f0
200 204 208 20a 20d 20e 20f 210 226 2a0 2c0 2c3 2c6 2c8 2dc 2e0 2e1 2e2 2f1 2f8 2fd 448 449 450 451 469 4b0 4b8
""".split())
# Original KanojoStep sysprg.arc: scrctrl1B0E, scrsys0C4B, scrgrp10737.
# These read stack arguments, never the inline-word helper scrctrl06A3.
# Bounded original disassembly: ghproj/kanojostep_translation/.
OBSERVED.update((0x34, 0x10B, 0x225))
OPERANDS = {0: 1, 1: 1, 2: 1, 3: 1, 8: 1, 9: 1, 10: 1,
            0x17: 1, 0x19: 1, 0x3F: 1, 0x7F: 2}
# Preserve complete tags, printf-style substitutions and control units exactly.
TOKENS = re.compile(r"主人公（(?:姓|名)）|<[^<>]*>|[<>]|%(?:\d+\$)?[-+#0 ]*\d*(?:\.\d+)?[hlL]?[a-zA-Z%]|[\x01-\x1f]")


def digest(data):
    return hashlib.sha256(data).hexdigest()


def jsonl(path):
    with path.open(encoding="utf-8") as handle:
        return [json.loads(line) for line in handle if line.strip()]


@dataclass
class Instruction:
    offset: int
    opcode: int
    operands: tuple

    def encode(self):
        return struct.pack("<" + "I" * (1 + len(self.operands)), self.opcode, *self.operands)


class CompiledScript:
    def __init__(self, payload, code_size=None):
        if not payload.startswith(MAGIC) or len(payload) < 36:
            raise ValueError("unsupported compiled-script header")
        self.payload = payload
        self.code_base = len(MAGIC) + struct.unpack_from("<I", payload, len(MAGIC))[0]
        if not 36 <= self.code_base <= len(payload):
            raise ValueError("compiled-script header is out of bounds")
        code = payload[self.code_base:]
        cursor, limit = 0, len(code) if code_size is None else code_size
        if code_size is not None and (not 0 <= limit <= len(code) or limit % 4):
            raise ValueError("invalid compiled instruction boundary")
        self.instructions = []
        references = []
        while cursor < limit:
            if cursor + 4 > len(code):
                raise ValueError("truncated word opcode")
            opcode = struct.unpack_from("<I", code, cursor)[0]
            if opcode not in OBSERVED:
                raise ValueError(f"unsupported compiled opcode {opcode:#x} at {cursor:#x}")
            size = OPERANDS.get(opcode, 0)
            if cursor + 4 * (size + 1) > limit:
                raise ValueError("word instruction overlaps string pool")
            operands = struct.unpack_from("<" + "I" * size, code, cursor + 4) if size else ()
            self.instructions.append(Instruction(cursor, opcode, operands))
            if opcode in (3, 0x7F):
                pointer = operands[0]
                if pointer < cursor + 4 * (size + 1) or pointer >= len(code):
                    raise ValueError("string reference points outside trailing pool")
                limit = min(limit, pointer)
                references.append(pointer)
            cursor += 4 * (size + 1)
        if cursor != limit or cursor % 4:
            raise ValueError("compiled code/pool boundary mismatch")
        self.code_size = cursor
        self.header = payload[:self.code_base]
        self.pool = code[self.code_size:]
        for pointer in references:
            if pointer < self.code_size or code.find(b"\0", pointer) < 0:
                raise ValueError("unterminated or code-overlapping string")
        boundaries = {instruction.offset for instruction in self.instructions}
        for instruction in self.instructions:
            if instruction.opcode == 1 and instruction.operands[0] not in boundaries:
                raise ValueError("typed code address does not name an instruction boundary")
        if self.rebuild() != payload:
            raise ValueError("unchanged script does not rebuild byte-for-byte")

    def rebuild(self):
        return self.header + b"".join(instruction.encode() for instruction in self.instructions) + self.pool

    def raw_string(self, pointer):
        start = self.code_base + pointer
        end = self.payload.find(b"\0", start)
        return self.payload[start:end]

    def text_records(self, archive, resource):
        records = []
        for index, instruction in enumerate(self.instructions):
            if instruction.opcode != 0x140:
                continue
            if index < 2 or self.instructions[index - 1].opcode != 3 or self.instructions[index - 2].opcode not in (0, 3):
                raise ValueError("unresolved message argument expression")
            for distance, role in ((2, "speaker"), (1, "dialogue")):
                argument = self.instructions[index - distance]
                if argument.opcode == 0:
                    if argument.operands != (0,):
                        raise ValueError("unresolved numeric speaker value")
                    continue
                raw = self.raw_string(argument.operands[0])
                source = raw.decode("cp932")
                # Setup's SetLineIcon uses the same character strings as lookup
                # keys. Keep every speaker argument immutable, even when the
                # fallback display label happens to look like ordinary prose.
                if role == "speaker":
                    role = "speaker_key"
                offset = self.code_base + argument.offset + 4
                records.append({"id": f"{archive}:{resource}:{offset:08x}", "archive": archive,
                                "resource": resource, "role": role, "operand_offset": offset,
                                "source_offset": self.code_base + argument.operands[0],
                                "call_offset": instruction.offset, "source_sha256": digest(raw),
                                "source": source, "target": None})
        # The original scripts pass appended text as: push_string, nargs(2),
        # native143. Selection strings are stored as local argument slots before
        # calling the imported _SelectEx function. Inspect decoded instructions,
        # not byte patterns that could coincide with integer operands.
        def add_record(argument, role, call_offset):
            raw = self.raw_string(argument.operands[0])
            source = raw.decode("cp932")
            offset = self.code_base + argument.offset + 4
            records.append({"id": f"{archive}:{resource}:{offset:08x}", "archive": archive,
                            "resource": resource, "role": role, "operand_offset": offset,
                            "source_offset": self.code_base + argument.operands[0],
                            "call_offset": call_offset, "source_sha256": digest(raw),
                            "source": source, "target": None})
        for index, instruction in enumerate(self.instructions):
            if instruction.opcode == 0x143:
                if index < 2 or self.instructions[index-1].opcode != 0x3F or self.instructions[index-2].opcode != 3:
                    raise ValueError("unresolved appended-message argument expression")
                add_record(self.instructions[index-2], "dialogue_append", instruction.offset)
            if instruction.opcode != 0x1C or index < 1 or self.instructions[index-1].opcode != 3:
                continue
            function = self.raw_string(self.instructions[index-1].operands[0])
            if function != b"_SelectEx":
                continue
            start = index-2
            while start >= 0 and self.instructions[start].opcode != 0xFE:
                if self.instructions[start].opcode == 0x1C:
                    raise ValueError("unresolved nested call within selection arguments")
                start -= 1
            if start < 0:
                raise ValueError("selection argument source boundary is unresolved")
            for pos in range(start+1, index-1):
                arg = self.instructions[pos]
                if arg.opcode != 3:
                    continue
                if pos == index-2:
                    add_record(arg, "choice_prompt", instruction.offset)
                elif pos >= 1 and self.instructions[pos-1].opcode == 2 and self.instructions[pos+1].opcode == 9:
                    add_record(arg, "choice", instruction.offset)
                else:
                    raise ValueError("selection string expression is unresolved")
        identifiers = [record["id"] for record in records]
        if len(identifiers) != len(set(identifiers)):
            raise ValueError("text operands overlap semantic roles")
        return sorted(records, key=lambda record: record["operand_offset"])


def normalize_changed_strings(original, replacement, encoding, errors="strict", overrides=None):
    """Convert translated operands without guessing encodings of original data.

    Both streams must have the same typed instruction layout. Original strings,
    including CP932 debug paths and internal keys, keep their bytes. Only string
    operands changed by the supplied translation use the explicit source codec.
    Append UTF-8 strings and relocate their typed pointers; do not shift code.
    """
    source, target = CompiledScript(original), CompiledScript(replacement)
    if len(source.instructions) != len(target.instructions):
        raise ValueError("translation instruction count differs from original")
    output, strings, allowed = bytearray(replacement), {}, set()
    report = {"source_encoding": encoding, "unchanged_operands": 0,
              "converted_operands": 0, "override_operands": 0, "invalid_text_operands": []}
    for before, after in zip(source.instructions, target.instructions):
        if (before.offset, before.opcode) != (after.offset, after.opcode):
            raise ValueError("translation instruction layout differs from original")
        if after.opcode not in (3, 0x7F):
            continue
        raw = target.raw_string(after.operands[0])
        override = (overrides or {}).get(digest(raw))
        if override is not None:
            encoded = override["text"].encode(override["encoding"])
            report["override_operands"] += 1
        elif raw == source.raw_string(before.operands[0]) or raw.isascii():
            report["unchanged_operands"] += 1
            continue
        else:
            try:
                text = raw.decode(encoding)
            except UnicodeDecodeError as error:
                if errors != "replace":
                    raise ValueError(f"invalid {encoding} text operand at {after.offset:#x}") from error
                text = raw.decode(encoding, errors="replace")
                report["invalid_text_operands"].append({"instruction_offset": after.offset,
                                                       "source_sha256": digest(raw)})
            encoded = text.encode("utf-8")
        if b"\0" in encoded:
            raise ValueError("text override contains NUL")
        if encoded == raw:
            continue
        if encoded not in strings:
            strings[encoded] = len(output) - target.code_base
            output.extend(encoded + b"\0")
        position = target.code_base + after.offset + 4
        struct.pack_into("<I", output, position, strings[encoded])
        allowed.update(range(position, position + 4))
        report["converted_operands"] += 1
    if any(output[index] != value for index, value in enumerate(replacement) if index not in allowed):
        raise ValueError("text normalization changed bytes outside typed string pointers")
    # Reparse the complete output: every relocated string and branch is checked.
    CompiledScript(bytes(output), code_size=target.code_size)
    return bytes(output), report


def inventory(args):
    if args.out.exists():
        raise ValueError("output workspace already exists")
    index = translation_patch.archive_index(args.game)
    targets = [(archive, name) for name, archives in index.items() for archive in archives
               if archive.startswith("data01") and len(archives) == 1]
    args.out.mkdir(parents=True)
    original = args.out / "original"
    original.mkdir()
    manifest = {"format": "BGI-COMPILED-TEXT-1", "scripts": []}
    records, unclassified = [], []
    for archive, name in sorted(targets):
        output = original / (name + ".bin")
        subprocess.run([str(args.engine.resolve()), "--game", str(args.game.resolve()),
                        "--no-translation-patch", "--dump-resource", archive + ":" + name,
                        "--dump-resource-output", str(output)], check=True, stdout=subprocess.DEVNULL)
        payload = output.read_bytes()
        if not payload.startswith(MAGIC):
            raise ValueError("unexpected non-compiled source in script archive")
        script = CompiledScript(payload)
        items = script.text_records(archive, name)
        known = {item["operand_offset"] for item in items}
        for instruction in script.instructions:
            if instruction.opcode != 3 or script.code_base + instruction.offset + 4 in known:
                continue
            raw = script.raw_string(instruction.operands[0])
            text = raw.decode("cp932")
            if any("\u3040" <= ch <= "\u9fff" for ch in text) and not text.startswith(("F:\\", "PrintMessage")):
                unclassified.append({"archive": archive, "resource": name,
                                     "operand_offset": script.code_base + instruction.offset + 4,
                                     "source": text})
        manifest["scripts"].append({"archive": archive, "resource": name,
                                    "original_file": str(output.relative_to(args.out)),
                                    "original_size": len(payload), "original_sha256": digest(payload),
                                    "code_base": script.code_base, "code_size": script.code_size,
                                    "instructions": len(script.instructions),
                                    "dialogue_count": sum(item["role"] == "dialogue" for item in items),
                                    "unchanged_rebuild_identical": script.rebuild() == payload})
        records.extend(items)
    (args.out / "manifest.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    for filename, items in (("texts.jsonl", records), ("unclassified.jsonl", unclassified)):
        (args.out / filename).write_text("".join(json.dumps(item, ensure_ascii=False) + "\n" for item in items), encoding="utf-8")
    print(f"Inventoried {len(targets)} scripts, {len(records)} speaker/dialogue operands; byte-identical baselines passed")


def build(args):
    manifest = json.loads((args.workspace / "manifest.json").read_text(encoding="utf-8"))
    entries, coverage = [], []
    translations = {}
    for item in jsonl(args.translations):
        if item["id"] in translations:
            raise ValueError("duplicate translation ID")
        if item.get("target") is not None:
            translations[item["id"]] = item
    used = set()
    for entry in manifest["scripts"]:
        payload = (args.workspace / entry["original_file"]).read_bytes()
        if digest(payload) != entry["original_sha256"]:
            raise ValueError("workspace original content changed")
        script = CompiledScript(payload)
        records = script.text_records(entry["archive"], entry["resource"])
        replacement, strings = bytearray(payload), {}
        counts = Counter()
        for record in records:
            item = translations.get(record["id"])
            if record["role"] == "speaker_key":
                if item is not None:
                    raise ValueError("speaker lookup keys are not translation targets")
                counts["preserved_speaker_key"] += 1
                continue
            if item is None:
                counts["untranslated_" + record["role"]] += 1
                continue
            if item.get("source_sha256") != record["source_sha256"] or item.get("source") != record["source"]:
                raise ValueError("translation source binding mismatch: " + record["id"])
            target = item["target"]
            if not isinstance(target, str) or "\0" in target:
                raise ValueError("invalid translated text")
            if TOKENS.findall(target) != TOKENS.findall(record["source"]):
                raise ValueError("translation changes control/tag/placeholder sequence: " + record["id"])
            encoding = item.get("target_encoding", "utf-8")
            if encoding not in ("utf-8", "cp932"):
                raise ValueError("unsupported target encoding: " + record["id"])
            used.add(item["id"])
            if re.search(r"主人公（(?:姓|名)）", record["source"]) and encoding != "cp932":
                # The original replacement keys and text-edit results are CP932.
                # A whole CP932 target preserves that byte contract; otherwise
                # retain the original rather than install mixed-encoding text.
                counts["blocked_name_encoding_" + record["role"]] += 1
                continue
            try:
                encoded = target.encode(encoding)
            except UnicodeEncodeError as error:
                raise ValueError("target is not representable in " + encoding + ": " + record["id"]) from error
            if encoding == "cp932":
                counts["cp932_text_count"] += 1
            if encoded not in strings:
                strings[encoded] = len(replacement) - script.code_base
                replacement.extend(encoded + b"\0")
            struct.pack_into("<I", replacement, record["operand_offset"], strings[encoded])
            used.add(item["id"])
            counts["translated_" + record["role"]] += 1
        # Every original byte outside the selected typed string operands remains
        # intact; unmodified header, pool, branch targets and exports are kept.
        allowed = {record["operand_offset"] + index for record in records if record["id"] in used for index in range(4)}
        if any(payload[index] != replacement[index] for index in range(len(payload)) if index not in allowed):
            raise ValueError("builder modified an unselected original byte")
        coverage.append({"archive": entry["archive"], "resource": entry["resource"], **counts})
        if any(key.startswith("translated_") and value for key, value in counts.items()):
            entries.append({"archive": entry["archive"], "resource": entry["resource"],
                            "original_size": len(payload), "original_sha256": digest(payload),
                            "payload": bytes(replacement)})
    if used != set(translations):
        raise ValueError("translation contains unknown or ineligible operand IDs")
    translation_patch.write_patch(entries, args.output)
    report = {"format": "BGI-COMPILED-TEXT-COVERAGE-1", "scope": "dialogue and choices; speaker/internal lookup keys preserved", "complete": all(not any((key.startswith("untranslated_") or key.startswith("blocked_")) and value for key, value in item.items()) for item in coverage),
              "scripts": coverage, "patch_sha256": digest(args.output.read_bytes())}
    args.output.with_suffix(args.output.suffix + ".coverage.json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(f"Built {len(entries)} script replacements; complete={report['complete']}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    extract = commands.add_parser("extract")
    extract.add_argument("--game", type=Path, required=True)
    extract.add_argument("--engine", type=Path, default=Path("build/bgiplayer"))
    extract.add_argument("--out", type=Path, required=True)
    pack = commands.add_parser("build")
    pack.add_argument("workspace", type=Path)
    pack.add_argument("--translations", type=Path, required=True)
    pack.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        (inventory if args.command == "extract" else build)(args)
    except (ValueError, OSError, struct.error, subprocess.CalledProcessError) as error:
        parser.exit(1, "compiled text patch: " + str(error) + "\n")

if __name__ == "__main__":
    main()
