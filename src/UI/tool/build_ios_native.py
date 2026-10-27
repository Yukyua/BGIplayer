#!/usr/bin/env python3
"""Build real static iOS engine dependencies in the repository build folder."""
import os,pathlib,subprocess,sys
root=pathlib.Path(__file__).resolve().parents[3]
mode=sys.argv[1] if len(sys.argv)>1 else 'simulator'
sdk='iphonesimulator' if mode=='simulator' else 'iphoneos'
triple='arm64-apple-ios15.0'+('-simulator' if mode=='simulator' else '')
base=root/'build/ios-native'; sources=base/'sources'; out=base/mode; prefix=out/'prefix'
if mode not in ('simulator','device'): raise SystemExit('expected simulator or device')
sources.mkdir(parents=True,exist_ok=True)
# Pin official upstream source releases; dependencies remain outside app sources.
import hashlib,tarfile
releases=[('sdl.tar.gz','https://www.libsdl.org/release/SDL2-2.32.10.tar.gz','SDL2-2.32.10'),('ffmpeg.tar.xz','https://ffmpeg.org/releases/ffmpeg-8.0.tar.xz','ffmpeg-8.0'),('freetype.tar.xz','https://download.savannah.gnu.org/releases/freetype/freetype-2.14.1.tar.xz','freetype-2.14.1')]
checksums={'sdl.tar.gz':'5f5993c530f084535c65a6879e9b26ad441169b3e25d789d83287040a9ca5165','ffmpeg.tar.xz':'b2751fccb6cc4c77708113cd78b561059b6fa904b24162fa0be2d60273d27b8e','freetype.tar.xz':'32427e8c471ac095853212a37aef816c60b42052d4d9e48230bab3bdf2936ccc'}
for archive,url,directory in releases:
 package=sources/archive
 if not package.exists(): subprocess.run(['curl','-L','--fail',url,'-o',str(package)],check=True)
 if hashlib.sha256(package.read_bytes()).hexdigest()!=checksums[archive]: raise RuntimeError('source checksum mismatch: '+archive)
 if not (sources/directory).exists():
  with tarfile.open(package) as bundle:
   for member in bundle.getmembers():
    target=(sources/member.name).resolve()
    if sources.resolve() not in target.parents: raise RuntimeError('unsafe source archive path')
   bundle.extractall(sources)

out.mkdir(parents=True,exist_ok=True);prefix.mkdir(exist_ok=True)
def run(args,cwd=None,env=None):subprocess.run([str(x) for x in args],cwd=cwd,env=env,check=True)
def output(args):return subprocess.check_output(args,text=True).strip()
sysroot=output(['xcrun','--sdk',sdk,'--show-sdk-path']);clang=output(['xcrun','--sdk',sdk,'--find','clang'])
flags=f'-target {triple} -isysroot {sysroot} -O2 -fPIC'
env=os.environ.copy();env.update(CC=clang,CFLAGS=flags,LDFLAGS=flags,AR=output(['xcrun','--find','ar']),RANLIB=output(['xcrun','--find','ranlib']))
if not (prefix/'lib/libfreetype.a').exists():
 d=out/'freetype';d.mkdir(exist_ok=True)
 run([sources/'freetype-2.14.1/configure','--host=arm-apple-darwin',f'--prefix={prefix}','--enable-static','--disable-shared','--without-harfbuzz','--without-brotli','--without-png','--without-bzip2','--without-zlib'],d,env)
 run(['make','-j4'],d,env);run(['make','install'],d,env)
if not (prefix/'lib/.ffmpeg-zlib-pngencoder-enabled').exists():
 d=out/'ffmpeg';d.mkdir(exist_ok=True)
 run([sources/'ffmpeg-8.0/configure',f'--prefix={prefix}','--target-os=darwin','--arch=aarch64','--enable-cross-compile',f'--cc={clang}',f'--sysroot={sysroot}',f'--extra-cflags={flags}',f'--extra-ldflags={flags}','--disable-asm','--disable-programs','--disable-doc','--disable-network','--disable-autodetect','--enable-zlib','--disable-everything','--enable-avformat','--enable-avcodec','--enable-swscale','--enable-swresample','--enable-protocol=file','--enable-encoder=png','--enable-decoder=h264,aac,vorbis,mp3,mp3float,pcm_s16le,pcm_s16be,pcm_u8,adpcm_ms,adpcm_ima_wav,wmv3,vc1,mpeg4,mpeg2video,mjpeg,png,bmp','--enable-demuxer=mov,ogg,wav,mp3,avi,asf,matroska,mpegps,mpegts','--enable-parser=h264,aac,mpegaudio,vorbis,mpeg4video,mpegvideo,vc1','--enable-static','--disable-shared'],d,env)
 run(['make','-j4'],d,env);run(['make','install'],d,env)
 (prefix/'lib/.ffmpeg-zlib-pngencoder-enabled').touch()
# SDL ships precompiled Metal shader headers; compiling an unused .metal build
# resource would require Xcode's optional Metal compiler download.
pbx=sources/'SDL2-2.32.10/Xcode/SDL/SDL.xcodeproj/project.pbxproj'
pbx.write_text(''.join(line for line in pbx.read_text().splitlines(True) if 'SDL_shaders_metal.metal in Sources' not in line))
window=sources/'SDL2-2.32.10/src/video/uikit/SDL_uikitwindow.m'
text=window.read_text()
old='uiwindow = [[SDL_uikitwindow alloc] initWithFrame:data.uiscreen.bounds];'
if 'BGI scene host' not in text:
 text=text.replace(old,old+'''
        // BGI scene host adaptation: Flutter already owns UIApplicationMain.
        if (@available(iOS 13.0, *)) {
            for (UIScene *scene in [UIApplication sharedApplication].connectedScenes) {
                if ([scene isKindOfClass:[UIWindowScene class]]) {
                    uiwindow.windowScene = (UIWindowScene *)scene;
                    break;
                }
            }
        }''')
 window.write_text(text)
if not (prefix/'lib/libSDL2.a').exists():
 run(['xcodebuild','-project',sources/'SDL2-2.32.10/Xcode/SDL/SDL.xcodeproj','-target','Static Library-iOS','-configuration','Release','-sdk',sdk,'ARCHS=arm64','ONLY_ACTIVE_ARCH=YES','CODE_SIGNING_ALLOWED=NO','IPHONEOS_DEPLOYMENT_TARGET=15.0',f'CONFIGURATION_BUILD_DIR={prefix}/lib',f'OBJROOT={out}/sdl-obj'])
# Preserve the shared VM and compositor implementation; only hosts differ.
print(prefix)
# Each aggregate .cpp retains its normal include ordering; desktop Cocoa hosts
# are replaced only by the UIKit boundary in BGIEnginePlugin.mm.
import concurrent.futures,re
from functools import lru_cache
obj=out/'engine-obj';obj.mkdir(exist_ok=True)
cxx=output(['xcrun','--sdk',sdk,'--find','clang++'])
includes=[f'-I{root}/include',f'-I{sources}/SDL2-2.32.10/include',f'-I{prefix}/include',f'-I{prefix}/include/freetype2']
cpp=[p for p in (root/'src').glob('*.cpp') if p.name!='main.cpp']+ [root/'src/gpu_pixel_kernels_metal.mm']
@lru_cache(None)
def dependencies(p):
 result={p}
 for name in re.findall(r'#\s*include\s*["<]([^">]+)',p.read_text(errors='ignore')):
  for candidate in (p.parent/name,root/'include'/name):
   if candidate.is_file():
    resolved=candidate.resolve()
    # Header cycles are common; traversal uses its own visited set below.
    result.add(resolved)
    break
 return result
def latest_dependency(p):
 seen=set();pending=[p];latest=0
 while pending:
  node=pending.pop()
  if node in seen:continue
  seen.add(node);latest=max(latest,node.stat().st_mtime)
  pending.extend(dependencies(node)-seen)
 return latest
def compile_one(p):
 target=obj/(p.name+'.o')
 if target.exists() and target.stat().st_mtime>=latest_dependency(p): return target
 run([cxx,'-std=c++20','-O1','-fPIC','-target',triple,'-isysroot',sysroot,*includes,'-c',p,'-o',target])
 return target
with concurrent.futures.ThreadPoolExecutor(max_workers=3) as pool:objects=list(pool.map(compile_one,cpp))
run(['xcrun','libtool','-static','-o',prefix/'lib/libbgiplayer.a',*objects])
