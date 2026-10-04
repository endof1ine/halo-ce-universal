#!/bin/sh
# Builds the FFmpeg the Switch's host decodes the game's movies with
# (port/switch/host/host_bink.c): only the Bink demuxer and decoders, the
# file protocol, the scaler and the resampler, under the LGPL (no
# --enable-gpl, unlike devkitPro's switch-ffmpeg). It installs to
# /opt/halo-ffmpeg (or $1), with its license. Needs devkitA64 on the PATH.

set -eu

prefix=${1:-/opt/halo-ffmpeg}
version=7.1.1
work=$(mktemp -d)

curl -fsSL "https://ffmpeg.org/releases/ffmpeg-$version.tar.xz" | tar -xJ -C "$work"
cd "$work/ffmpeg-$version"
./configure --prefix="$prefix" \
	--enable-cross-compile --cross-prefix=aarch64-none-elf- --arch=aarch64 --cpu=cortex-a57 \
	--target-os=none --disable-pthreads --enable-pic --enable-static --disable-shared \
	--extra-cflags="-D__SWITCH__ -D_GNU_SOURCE -O2 -march=armv8-a -mtune=cortex-a57 -mtp=soft -fPIC -ftls-model=local-exec -I$DEVKITPRO/libnx/include" \
	--extra-ldflags="-fPIE -L$DEVKITPRO/libnx/lib" \
	--disable-programs --disable-doc --disable-debug --disable-autodetect --disable-runtime-cpudetect \
	--disable-everything --disable-avdevice --disable-avfilter --disable-network \
	--enable-demuxer=bink --enable-decoder=bink,binkaudio_dct,binkaudio_rdft --enable-protocol=file \
	--enable-swscale --enable-swresample --enable-neon --enable-asm \
	>"$work/configure.log" || { tail -n 30 "$work/configure.log" ffbuild/config.log; exit 1; }
make -j"$(nproc)" >"$work/make.log" 2>&1 || { tail -n 40 "$work/make.log"; exit 1; }
make install >/dev/null
cp COPYING.LGPLv2.1 "$prefix/COPYING.LGPLv2.1"
rm -rf "$work"
echo "FFmpeg $version (LGPL, Bink only) in $prefix"
