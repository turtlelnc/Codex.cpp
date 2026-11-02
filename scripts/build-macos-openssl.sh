#!/bin/sh
# Static TLS built for the advertised deployment target, not Homebrew's host OS.
set -eu
case "$1" in
  arm64) target=darwin64-arm64-cc ;;
  x86_64) target=darwin64-x86_64-cc ;;
  *) echo 'Unsupported macOS architecture' >&2; exit 2 ;;
esac
prefix=$2
work=$(mktemp -d "${RUNNER_TEMP:-/tmp}/codex-openssl.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
curl -fLsS --retry 3 https://github.com/openssl/openssl/releases/download/openssl-3.5.9/openssl-3.5.9.tar.gz -o "$work/openssl.tar.gz"
(cd "$work" && echo '603f5602e2eef00d77fbd429d34dcd5822bb301757a1bc9cdb24c670f1eb859a  openssl.tar.gz' | shasum -a 256 -c -)
tar -xzf "$work/openssl.tar.gz" -C "$work"
cd "$work/openssl-3.5.9"
export MACOSX_DEPLOYMENT_TARGET=12.0
./Configure "$target" no-shared no-tests no-module no-zlib no-zstd no-brotli --prefix="$prefix" --libdir=lib -mmacosx-version-min=12.0
make -j3 build_sw
make install_sw
