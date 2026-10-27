#!/usr/bin/env python3
"""Build an exact-version PE64 launcher from a locally owned BGITRP01 patch.
Generated binaries contain game data: keep the output outside version control.
"""
import argparse
import hashlib
import json
import re
from pathlib import Path
import struct
import subprocess
import tempfile
import zlib


def read_patch(path):
    data = Path(path).read_bytes()
    if data[:8] != b'BGITRP01' or not 12 <= len(data) <= 256*1024*1024:
        raise ValueError('invalid translation package')
    count, = struct.unpack_from('<I', data, 8)
    if not 0 < count <= 4096:
        raise ValueError('invalid entry count')
    cursor = 12
    entries = {}
    decoded_total = 0
    for _ in range(count):
        if cursor + 80 > len(data):
            raise ValueError('truncated metadata')
        a, n, size, source_sha, new_size, stored, new_sha = struct.unpack_from('<HHI32sII32s', data, cursor)
        cursor += 80
        decoded_total += new_size
        if not 0 < new_size <= 64*1024*1024 or decoded_total > 256*1024*1024:
            raise ValueError('decoded package exceeds resource budget')
        if not a or not n or max(size, new_size, stored) > 256 * 1024 * 1024 or cursor+a+n+stored > len(data):
            raise ValueError('invalid entry bounds')
        archive = data[cursor:cursor+a].decode('ascii'); cursor += a
        name = data[cursor:cursor+n].decode('ascii'); cursor += n
        if Path(archive).name != archive or any(c not in 'abcdefghijklmnopqrstuvwxyz0123456789_.-' for c in archive):
            raise ValueError('invalid archive name')
        inflater = zlib.decompressobj()
        payload = inflater.decompress(data[cursor:cursor+stored], new_size+1)
        if not inflater.eof or inflater.unused_data or len(payload) != new_size or hashlib.sha256(payload).digest() != new_sha:
            raise ValueError('replacement checksum/size mismatch')
        cursor += stored
        key = (archive, name)
        if key in entries:
            raise ValueError('duplicate resource')
        entries[key] = (size, source_sha, payload)
    if cursor != len(data):
        raise ValueError('trailing package bytes')
    return entries


def checksum(data):
    result = bytearray(8)
    value = 0
    for byte in data:
        value = (value*0xe9+byte) & 0xffffffff
        result[4] = (result[4]+(value & 255)) & 255
        result[5] ^= value & 255
        result[6] = (result[6]+byte) & 255
        result[7] ^= byte
    result[:4] = struct.pack('<I', value)
    return bytes(result)


def rebuild_arc(data, replacements):
    if data[:12] != b'BURIKO ARC20' or len(data) < 16:
        raise ValueError('only observed ARC20 archive layout is supported')
    count, = struct.unpack_from('<I', data, 12)
    start = 16 + count*128
    if start > len(data):
        raise ValueError('truncated archive table')
    header = bytearray(data[:start]); body = bytearray(); changed = set(); names = set()
    for i in range(count):
        entry = 16+i*128
        raw_name = data[entry:entry+96].split(b'\0', 1)[0]
        name = raw_name.decode('ascii').lower()
        if name in names:
            raise ValueError('duplicate archive entry')
        names.add(name)
        offset, size = struct.unpack_from('<II', data, entry+96)
        if offset+size > len(data)-start:
            raise ValueError('archive entry out of bounds')
        payload = data[start+offset:start+offset+size]
        if name in replacements:
            payload = replacements[name]
            header[entry+104:entry+112] = checksum(payload)
            changed.add(name)
        struct.pack_into('<II', header, entry+96, len(body), len(payload))
        body.extend(payload)
    if changed != set(replacements):
        raise ValueError('replacement resource absent from source archive')
    return bytes(header+body)


def c_wide(text):
    encoded = text.encode('utf-16le')
    return '{'+','.join(str(x[0]) for x in struct.iter_unpack('<H', encoded))+',0}'


def c_sha(data):
    return '{'+','.join(str(b) for b in hashlib.sha256(data).digest())+'}'


def build(args):
    game = args.game.resolve(); output = args.output.resolve()
    exe = args.exe.resolve()
    if not exe.is_file() or exe.parent != game:
        raise ValueError('--exe must be the original executable in --game')
    exe_data = exe.read_bytes(); pe = struct.unpack_from('<I', exe_data, 60)[0]
    if exe_data[:2] != b'MZ' or exe_data[pe:pe+6] != b'PE\0\0\x64\x86':
        raise ValueError('launcher supports only original Windows PE64 engines')
    entries = read_patch(args.patch)
    output.mkdir(parents=True, exist_ok=True)
    header = ['#include <windows.h>', 'static const wchar_t game_exe[]='+c_wide(exe.name)+';',
              'static const unsigned char exe_sha[32]='+c_sha(exe_data)+';']
    archives = []
    manifest = {'format': 'BGI-WINDOWS-TRANSLATION-1', 'exe': exe.name,
                'exe_sha256': hashlib.sha256(exe_data).hexdigest(),
                'patch_sha256': hashlib.sha256(args.patch.read_bytes()).hexdigest(), 'archives': []}
    with tempfile.TemporaryDirectory(prefix='bgi-original-') as temporary:
        for i, archive in enumerate(sorted({key[0] for key in entries})):
            replacements = {}
            for (arc, name), (size, sha, payload) in entries.items():
                if arc != archive:
                    continue
                decoded = Path(temporary)/'decoded.bin'
                subprocess.run([str(args.engine.resolve()), '--game', str(game), '--no-translation-patch',
                                '--dump-resource', arc+':'+name, '--dump-resource-output', str(decoded)],
                               check=True, stdout=subprocess.DEVNULL)
                original = decoded.read_bytes()
                if len(original) != size or hashlib.sha256(original).digest() != sha:
                    raise ValueError('original resource binding mismatch: '+arc+':'+name)
                replacements[name] = payload
            original_arc = (game/'Archive'/archive).read_bytes()
            rebuilt = rebuild_arc(original_arc, replacements)
            (output/archive).write_bytes(rebuilt)
            header.append('static const wchar_t arc_name_'+str(i)+'[]='+c_wide(archive)+';')
            archives.append('{arc_name_'+str(i)+','+c_sha(original_arc)+'}')
            manifest['archives'].append({'name': archive, 'source_sha256': hashlib.sha256(original_arc).hexdigest(),
                                        'overlay_sha256': hashlib.sha256(rebuilt).hexdigest(),
                                        'replacements': sorted(replacements)})
    header += ['#define ARCHIVE_COUNT '+str(len(archives)),
               'static const struct { const wchar_t*name; unsigned char sha[32]; } archives[]={'+','.join(archives)+'};']
    (output/'translation_manifest.h').write_text('\n'.join(header)+'\n')
    source = Path(__file__).with_name('windows_translation_loader.c').resolve()
    cc = args.prefix+'gcc'; windres = args.prefix+'windres'
    subprocess.run([cc, '-mcrtdll=msvcrt-os', '-O2', '-Wall', '-Wextra', '-I', str(output), '-DBUILD_DLL', '-shared',
                    str(source), '-lgdi32', '-o', str(output/'translation-loader.dll')], check=True)
    resource = ['10 RCDATA "translation-loader.dll"']
    resource += [f'{100+i} RCDATA "{entry["name"]}"' for i, entry in enumerate(manifest['archives'])]
    (output/'translation.rc').write_text('\n'.join(resource)+'\n')
    subprocess.run([windres, 'translation.rc', '-O', 'coff', '-o', 'translation-res.o'], cwd=output, check=True)
    subprocess.run([cc, '-mcrtdll=msvcrt-os', '-O2', '-Wall', '-Wextra', '-municode', '-I', str(output), str(source),
                    str(output/'translation-res.o'), '-lbcrypt', '-o', str(output/'BGI_Chinese.exe')], check=True)
    # CrossOver supplies private UCRT API sets absent from some native systems.
    # Assert the actual PE imports rather than trusting the compiler CRT option.
    manifest['runtime_imports'] = {}
    allowed = {'gdi32.dll', 'kernel32.dll', 'msvcrt.dll', 'bcrypt.dll', 'user32.dll'}
    for binary in ['BGI_Chinese.exe', 'translation-loader.dll']:
        imports = subprocess.check_output([args.prefix+'objdump', '-p', str(output/binary)], text=True)
        names = sorted(set(re.findall(r'DLL Name: (\S+)', imports)))
        if not names or any(name.lower() not in allowed for name in names):
            raise ValueError('unexpected Windows runtime dependency in '+binary+': '+','.join(names))
        manifest['runtime_imports'][binary] = names
    manifest['launcher_sha256'] = hashlib.sha256((output/'BGI_Chinese.exe').read_bytes()).hexdigest()
    (output/'manifest.json').write_text(json.dumps(manifest, ensure_ascii=False, indent=2)+'\n')
    print(output/'BGI_Chinese.exe')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--game', type=Path, required=True)
    parser.add_argument('--exe', type=Path, required=True)
    parser.add_argument('--patch', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--engine', type=Path, default=Path('build/bgiplayer'))
    parser.add_argument('--prefix', default='x86_64-w64-mingw32-')
    try:
        build(parser.parse_args())
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        parser.exit(1, str(error)+'\n')

if __name__ == '__main__':
    main()
