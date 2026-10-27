"""Bundle the already built native engine and its non-system dylib closure."""
from pathlib import Path
import shutil
import subprocess
import sys

engine, app = map(lambda arg: Path(arg).resolve(), sys.argv[1:3])
if not engine.is_file() or not app.is_dir():
    raise SystemExit('Usage: bundle_engine.py EXISTING_ENGINE APP_BUNDLE')
folder = app / 'Contents' / 'Resources' / 'engine'
folder.mkdir(parents=True, exist_ok=True)
output = folder / 'bgiplayer'
shutil.copy2(engine, output)
libdir = folder / 'lib'
libdir.mkdir(exist_ok=True)
seen = {}

def run(*args):
    return subprocess.check_output(args, text=True)

def package(original, copied):
    for row in run('otool', '-L', str(original)).splitlines()[1:]:
        reference = row.strip().split(' (compatibility')[0]
        if reference.startswith(('/System/', '/usr/lib/')):
            continue
        if reference.startswith('@loader_path/'):
            source = original.parent / reference[len('@loader_path/'):]
        elif reference.startswith('/'):
            source = Path(reference)
        else:
            raise SystemExit(f'Unresolved dependency {reference} from {original}')
        source = source.resolve()
        # dylib's own install name is the first row in otool -L.
        if source == original.resolve():
            continue
        if not source.is_file():
            raise SystemExit(f'Missing dependency: {source}')
        target = libdir / source.name
        if target.name in seen and seen[target.name] != source:
            raise SystemExit(f'Dylib name collision: {source}')
        if target.name not in seen:
            seen[target.name] = source
            shutil.copy2(source, target)
            subprocess.run(['chmod', 'u+w', str(target)], check=True)
            subprocess.run(['install_name_tool', '-id', '@loader_path/' + target.name, str(target)], check=True)
            package(source, target)
        relative = ('lib/' if copied == output else '') + target.name
        subprocess.run(['install_name_tool', '-change', reference, '@loader_path/' + relative, str(copied)], check=True)
    subprocess.run(['codesign', '--force', '--sign', '-', str(copied)], check=True)

package(engine, output)
# Homebrew's SDL2 package is SDL2-compat: SDL3 is opened dynamically and
# therefore absent from otool -L. Its actual loader searches this basename.
if any(b'@loader_path/libSDL3.dylib\x00' in source.read_bytes()
       for source in seen.values() if source.name.startswith('libSDL2')):
    sdl3dir = Path(run('pkg-config', '--variable=libdir', 'sdl3').strip())
    source = (sdl3dir / 'libSDL3.dylib').resolve()
    if not source.is_file():
        raise SystemExit('SDL2-compat requires libSDL3.dylib for its dynamic loader')
    target = libdir / 'libSDL3.dylib'
    shutil.copy2(source, target)
    subprocess.run(['chmod', 'u+w', str(target)], check=True)
    subprocess.run(['install_name_tool', '-id', '@loader_path/' + target.name, str(target)], check=True)
    package(source, target)
    seen[target.name] = source
for binary in [output, *libdir.glob('*.dylib')]:
    subprocess.run(['codesign', '--verify', '--strict', str(binary)], check=True)
subprocess.run(['codesign', '--force', '--deep', '--sign', '-', str(app)], check=True)
print(f'Bundled engine and {len(seen)} libraries into {app.name}')
