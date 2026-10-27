#!/usr/bin/env python3
"""Wrap the unsigned device app in a standard IPA; never signs or uploads."""
from pathlib import Path
import os,plistlib,stat,sys,zipfile
ui=Path(__file__).resolve().parents[1]
app=Path(sys.argv[1]) if len(sys.argv)>1 else ui/'build/ios/iphoneos/Runner.app'
output=Path(sys.argv[2]) if len(sys.argv)>2 else ui/'build/ios/BGIplayer-unsigned.ipa'
info=plistlib.loads((app/'Info.plist').read_bytes())
if info.get('DTPlatformName')!='iphoneos' or 'iPhoneOS' not in info.get('CFBundleSupportedPlatforms',[]):
 raise SystemExit('IPA requires an iPhoneOS device build, never a simulator build')
if not (app/info['CFBundleExecutable']).is_file():raise SystemExit('missing app executable')
output.parent.mkdir(parents=True,exist_ok=True)
with zipfile.ZipFile(output,'w',compression=zipfile.ZIP_DEFLATED,compresslevel=6) as archive:
 for path in sorted(app.rglob('*')):
  name='Payload/BGIplayer.app/'+path.relative_to(app).as_posix()
  if path.is_symlink():
   entry=zipfile.ZipInfo(name);entry.create_system=3
   entry.external_attr=(stat.S_IFLNK|0o777)<<16
   archive.writestr(entry,os.readlink(path))
  elif path.is_file():archive.write(path,name)
with zipfile.ZipFile(output) as archive:
 if archive.testzip() is not None:raise SystemExit('IPA CRC validation failed')
 archived=plistlib.loads(archive.read('Payload/BGIplayer.app/Info.plist'))
 if archived!=info:raise SystemExit('IPA Info.plist changed')
 if f"Payload/BGIplayer.app/{info['CFBundleExecutable']}" not in archive.namelist():raise SystemExit('IPA executable missing')
 if any(not n.startswith('Payload/BGIplayer.app/') for n in archive.namelist()):raise SystemExit('invalid IPA root path')
print(f'{output} ({output.stat().st_size} bytes; unsigned device package)')
