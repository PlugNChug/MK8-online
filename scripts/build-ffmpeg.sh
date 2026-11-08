#!/bin/bash
# Minimal FFmpeg for the runtime's movie playback, cross-compiled for Windows in WSL (see scripts/build-ffmpeg.py).
# Every download is checked against the SHA-256 below. Output: $W/out (4 DLLs, license files, source archive).
set -euo pipefail
# A neutral folder: FFmpeg stores its configure line (with these paths) inside the DLLs.
W=/tmp/mk8-ffmpeg
FFMPEG_SHA=8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e
mkdir -p "$W/src" && cd "$W/src"
fetch() { # url sha file
  if [ -f "$3" ] && echo "$2  $3" | sha256sum -c --quiet 2>/dev/null; then return; fi
  curl -L --fail --silent --show-error -o "$3.part" "$1"
  echo "$2  $3.part" | sha256sum -c --quiet
  mv "$3.part" "$3"
}
fetch https://ziglang.org/download/0.17.0/zig-x86_64-linux-0.17.0.tar.xz 1cbe9df9f27e6b78d14ccbca43b6703a404ef79ef1c463de901d7f088d4e2026 zig.tar.xz
fetch https://www.nasm.us/pub/nasm/releasebuilds/2.16.03/nasm-2.16.03.tar.xz 1412a1c760bbd05db026b6c0d1657affd6631cd0a63cddb6f73cc6d4aa616148 nasm.tar.xz
fetch https://ffmpeg.org/releases/ffmpeg-9.0.2.tar.xz $FFMPEG_SHA ffmpeg-9.0.2.tar.xz
cd "$W"
[ -x zig/zig ] || { rm -rf zig; mkdir zig; tar -xJf src/zig.tar.xz -C zig --strip-components=1; }
if [ ! -x nasm/bin/nasm ]; then
  rm -rf nasm-build; mkdir nasm-build; tar -xJf src/nasm.tar.xz -C nasm-build --strip-components=1
  (cd nasm-build && ./configure --prefix="$W/nasm" >/dev/null && make -j"$(nproc)" >/dev/null && make install >/dev/null)
fi
# Zig's linker has no --pic-executable (FFmpeg adds it for mingw64 programs; the libraries do not use it).
mkdir -p bin
cat > bin/zcc <<'WRAP'
#!/bin/bash
args=()
for a in "$@"; do
  a="${a//--pic-executable,/}"
  [ "$a" = "-Wl,--pic-executable" ] && continue
  args+=("$a")
done
exec /tmp/mk8-ffmpeg/zig/zig cc -target x86_64-windows-gnu "${args[@]}"
WRAP
chmod +x bin/zcc
rm -rf ffmpeg-build out
mkdir ffmpeg-build out
tar -xJf src/ffmpeg-9.0.2.tar.xz -C ffmpeg-build --strip-components=1
cd ffmpeg-build
export ZIG_GLOBAL_CACHE_DIR="$W/zig-cache" ZIG_LOCAL_CACHE_DIR="$W/zig-cache"
# The movies are H.264 + AAC in MP4 and VP8 in WebM. LGPL only: no GPL or non-free component is enabled.
./configure --prefix="$W/install" --target-os=mingw32 --arch=x86_64 --enable-cross-compile \
  --cc="$W/bin/zcc" --cxx="$W/zig/zig c++ -target x86_64-windows-gnu" \
  --ld="$W/bin/zcc" --ar="$W/zig/zig ar" --ranlib="$W/zig/zig ranlib" --nm="nm" \
  --x86asmexe="$W/nasm/bin/nasm" --host-cc=gcc --pkg-config=false \
  --enable-shared --disable-static --disable-programs --disable-doc --disable-debug --disable-stripping \
  --disable-autodetect --disable-network --disable-everything \
  --disable-avdevice --disable-avfilter --disable-swresample \
  --enable-avcodec --enable-avformat --enable-avutil --enable-swscale --enable-w32threads \
  --enable-demuxer=mov,matroska --enable-decoder=h264,vp8,aac --enable-parser=h264,vp8,aac \
  --enable-protocol=file --extra-ldflags=-s > "$W/configure.log" 2>&1 || { tail -30 "$W/configure.log"; exit 1; }
grep -q "^x86 assembler *$W/nasm/bin/nasm" "$W/configure.log" || { echo "assembly x86 desativado"; exit 1; }
grep -q "^threading support *w32threads" "$W/configure.log" || { echo "threads ausentes"; exit 1; }
make -j"$(nproc)" > "$W/make.log" 2>&1 || { tail -40 "$W/make.log"; exit 1; }
# Only the DLLs: the runtime links the pinned MSYS2 headers and import libraries of this same version.
cp libavutil/avutil-61.dll libavcodec/avcodec-63.dll libavformat/avformat-63.dll libswscale/swscale-10.dll "$W/out/"
cp LICENSE.md COPYING.LGPLv2.1 "$W/out/"
cp "$W/src/ffmpeg-9.0.2.tar.xz" "$W/out/"
grep -m1 "^# FFMPEG_CONFIGURATION\|^#define FFMPEG_CONFIGURATION" config.h | cut -c1-2000 > "$W/out/configuration.txt" || true
ME="$(whoami)"
if [ -n "$ME" ] && grep -a -l -e "$HOME" -e "$ME" "$W"/out/*.dll; then echo "DLL com caminho do usuário"; exit 1; fi
