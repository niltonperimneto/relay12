#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Compile both frontends from an already patched, pinned Wine tree. Keeping
# this separate from patch application makes the same gate usable on a Mac.
set -eu
source_dir=$(CDPATH= cd -- "${1:?usage: build-wine-d3d11-host.sh WINE_TREE BUILD_DIR}" && pwd)
mkdir -p "${2:?missing build directory}"
build_dir=$(CDPATH= cd -- "$2" && pwd)
cd "$build_dir"
# Wine's compile-only configure probes also pass linker options to Clang.
# Ignore that driver-only warning while retaining -Werror for source warnings.
CROSSCFLAGS='-O2 -Werror -Wno-unused-command-line-argument' "$source_dir/configure" \
    --enable-archs=x86_64 --disable-tests \
    --without-x --without-wayland --without-gstreamer \
    --without-oss --without-alsa --without-pulse --without-sane \
    --without-usb --without-v4l2 --without-pcap --without-capi \
    --without-opencl --without-ffmpeg --without-cups --without-freetype
make -j"${RELAY12_BUILD_JOBS:-2}" dlls/d3d11/all dlls/d3d11on12host/all
test -s dlls/d3d11/x86_64-windows/d3d11.dll
test -s dlls/d3d11on12host/x86_64-windows/d3d11on12host.dll
