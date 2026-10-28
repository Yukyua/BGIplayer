#!/usr/bin/env python3
"""Pinned Android arm64 dependencies; engine/SDL shared libraries use app CMake."""
import argparse, hashlib, os, pathlib, platform, subprocess, tarfile
ROOT=pathlib.Path(__file__).resolve().parents[3]
p=argparse.ArgumentParser();p.add_argument('--sdk',default=os.environ.get('ANDROID_SDK_ROOT',str(ROOT/'build/android-sdk')));p.add_argument('--jobs',default='4');a=p.parse_args()
SDK=pathlib.Path(a.sdk).resolve();BASE=ROOT/'build/android-native';SRC=BASE/'sources';OUT=BASE/'arm64-v8a';PREFIX=OUT/'prefix'
NDK=SDK/'ndk/28.2.13676358';TC=NDK/'toolchains/llvm/prebuilt'/('darwin-x86_64' if platform.system()=='Darwin' else 'linux-x86_64');BIN=TC/'bin';CC=BIN/'aarch64-linux-android28-clang';CXX=BIN/'aarch64-linux-android28-clang++'
for d in (SRC,OUT,PREFIX):d.mkdir(parents=True,exist_ok=True)
def run(args,cwd=None,env=None):subprocess.run([str(x) for x in args],cwd=cwd,env=env,check=True)
releases=[('SDL2-2.32.10.tar.gz','https://www.libsdl.org/release/SDL2-2.32.10.tar.gz','SDL2-2.32.10','5f5993c530f084535c65a6879e9b26ad441169b3e25d789d83287040a9ca5165'),('ffmpeg-8.0.tar.xz','https://ffmpeg.org/releases/ffmpeg-8.0.tar.xz','ffmpeg-8.0','b2751fccb6cc4c77708113cd78b561059b6fa904b24162fa0be2d60273d27b8e'),('freetype-2.14.1.tar.xz','https://download.savannah.gnu.org/releases/freetype/freetype-2.14.1.tar.xz','freetype-2.14.1','32427e8c471ac095853212a37aef816c60b42052d4d9e48230bab3bdf2936ccc'),('libiconv-1.18.tar.gz','https://ftp.gnu.org/pub/gnu/libiconv/libiconv-1.18.tar.gz','libiconv-1.18','3b08f5f4f9b4eb82f151a7040bfd6fe6c6fb922efe4b1659c66ea933276965e8')]
for name,url,directory,sha in releases:
 archive=SRC/name
 if not archive.exists():
  cached=ROOT/'build/ios-native/sources'/({'SDL2-2.32.10.tar.gz':'sdl.tar.gz','ffmpeg-8.0.tar.xz':'ffmpeg.tar.xz','freetype-2.14.1.tar.xz':'freetype.tar.xz'}.get(name,name))
  if cached.exists():import shutil;shutil.copyfile(cached,archive)
  else:run(['curl','-L','--fail',url,'-o',archive])
 digest=hashlib.sha256(archive.read_bytes()).hexdigest()
 if sha and digest!=sha:raise RuntimeError('source checksum mismatch: '+name)
 if not (SRC/directory).exists():
  with tarfile.open(archive) as tar:
   for m in tar.getmembers():
    if SRC.resolve() not in (SRC/m.name).resolve().parents:raise RuntimeError('unsafe archive')
   tar.extractall(SRC)
 print(name,digest)
env=os.environ.copy();env.update(CC=str(CC),CXX=str(CXX),AR=str(BIN/'llvm-ar'),RANLIB=str(BIN/'llvm-ranlib'),STRIP=str(BIN/'llvm-strip'),CFLAGS='-O2 -fPIC',CXXFLAGS='-O2 -fPIC')
for name,options,lib in [('libiconv-1.18',['--disable-nls','--enable-extra-encodings'],'libiconv.a'),('freetype-2.14.1',['--without-harfbuzz','--without-brotli','--without-png','--without-bzip2','--without-zlib'],'libfreetype.a')]:
 if not (PREFIX/'lib'/lib).exists():
  d=OUT/name;d.mkdir(exist_ok=True);run([SRC/name/'configure','--host=aarch64-linux-android',f'--prefix={PREFIX}','--enable-static','--disable-shared',*options],d,env);run(['make','-j'+a.jobs],d,env);run(['make','install'],d,env)
if not (PREFIX/'lib/libavcodec.a').exists():
 d=OUT/'ffmpeg';d.mkdir(exist_ok=True)
 run([SRC/'ffmpeg-8.0/configure',f'--prefix={PREFIX}','--target-os=android','--arch=aarch64','--enable-cross-compile',f'--cc={CC}',f'--cxx={CXX}',f'--ar={BIN}/llvm-ar',f'--ranlib={BIN}/llvm-ranlib',f'--strip={BIN}/llvm-strip',f'--sysroot={TC}/sysroot','--extra-cflags=-O2 -fPIC','--disable-asm','--disable-programs','--disable-doc','--disable-network','--disable-autodetect','--enable-zlib','--disable-everything','--enable-avformat','--enable-avcodec','--enable-swscale','--enable-swresample','--enable-protocol=file','--enable-encoder=png','--enable-decoder=h264,aac,vorbis,mp3,mp3float,pcm_s16le,pcm_s16be,pcm_u8,adpcm_ms,adpcm_ima_wav,wmv3,vc1,mpeg4,mpeg2video,mjpeg,png,bmp','--enable-demuxer=mov,ogg,wav,mp3,avi,asf,matroska,mpegps,mpegts','--enable-parser=h264,aac,mpegaudio,vorbis,mpeg4video,mpegvideo,vc1','--enable-static','--disable-shared'],d,env)
 run(['make','-j'+a.jobs],d,env);run(['make','install'],d,env)
print('Android dependency prefix:',PREFIX)
