#!/bin/sh
# Run inside Alpine on the target architecture; Python/Meson are build tools only.
set -eu
apk add --no-cache build-base cmake ninja openssl-dev openssl-libs-static zlib-static zstd-static linux-headers libcap-dev libcap-static meson pkgconf curl git file
curl -fsSL https://codeload.github.com/containers/bubblewrap/tar.gz/refs/tags/v0.11.0 -o /tmp/bubblewrap.tar.gz
echo 'cfeeb15fcc47d177d195f06fdf0847e93ee3aa6bf46f6ac0a141fa142759e2c3  /tmp/bubblewrap.tar.gz' | sha256sum -c -
tar -xzf /tmp/bubblewrap.tar.gz -C /tmp
LDFLAGS=-static meson setup /tmp/bwrap-build /tmp/bubblewrap-0.11.0 --buildtype=release --default-library=static -Dselinux=disabled -Dtests=false -Dman=disabled -Dbash_completion=disabled -Dzsh_completion=disabled
meson compile -C /tmp/bwrap-build
file /tmp/bwrap-build/bwrap | grep -E 'statically linked|static-pie linked'
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXE_LINKER_FLAGS=-static -DCODEX_BWRAP_EXECUTABLE=/tmp/bwrap-build/bwrap -DCODEX_EXTRA_LICENSE_FILES=/tmp/bubblewrap-0.11.0/COPYING
cmake --build build-release --target codex-cpp --parallel 2
strip build-release/codex-cpp
