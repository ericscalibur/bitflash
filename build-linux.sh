#!/usr/bin/env bash
# Bitflash headless node/miner -- one-shot Linux build (Debian/Ubuntu).
# Builds the dependencies (libsecp256k1 with schnorrsig, RandomX) and then the
# node itself. Run from the project root (the directory that contains src/):
#
#   bash build-linux.sh
#
# Result: src/bitflash-node . Run it with:  ./src/bitflash-node -gen

set -e
ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"

# Use sudo only when not already root (a root login often has no sudo installed).
SUDO=""
if [ "$(id -u)" -ne 0 ]; then SUDO="sudo"; fi

echo "==> installing system packages"
if command -v apt-get >/dev/null 2>&1; then
  $SUDO apt-get update
  $SUDO apt-get install -y build-essential cmake git pkg-config autoconf libtool \
       libssl-dev libdb5.3++-dev libsodium-dev nlohmann-json3-dev libboost-dev \
       libboost-system-dev libwxgtk3.2-dev
else
  echo "!! non-apt system: install manually -> g++ cmake git autoconf libtool"
  echo "   libssl-dev libdb++-dev libsodium-dev nlohmann-json (headers)"
fi

# ---- libsecp256k1 (Schnorr/BIP340 + extrakeys), static, installed to /usr/local
if [ ! -f /usr/local/lib/libsecp256k1.a ]; then
  echo "==> building libsecp256k1"
  rm -rf "$ROOT/secp256k1-build"
  git clone --depth 1 https://github.com/bitcoin-core/secp256k1 "$ROOT/secp256k1-build"
  cd "$ROOT/secp256k1-build"
  ./autogen.sh
  ./configure --enable-module-schnorrsig --enable-module-extrakeys \
              --disable-shared --with-pic --disable-benchmark --disable-tests
  make -j"$(nproc)"
  $SUDO make install
  $SUDO ldconfig
  cd "$ROOT"
else
  echo "==> libsecp256k1 already installed, skipping"
fi

# ---- RandomX (memory-hard CPU PoW), static lib
if [ ! -f "$HOME/RandomX/build/librandomx.a" ]; then
  echo "==> building RandomX"
  rm -rf "$HOME/RandomX"
  git clone --depth 1 https://github.com/tevador/RandomX "$HOME/RandomX"
  mkdir -p "$HOME/RandomX/build"
  cd "$HOME/RandomX/build"
  cmake .. -DCMAKE_BUILD_TYPE=Release
  make -j"$(nproc)" randomx
  cd "$ROOT"
else
  echo "==> RandomX already built, skipping"
fi

# ---- build
echo "==> building bitflash"
cd "$ROOT/src"
make -f makefile.linux RANDOMX_DIR="$HOME/RandomX" -j"$(nproc)"

echo
echo "==================================================================="
echo " DONE.  $ROOT/src/bitflash"
echo
echo "   GUI:         ./src/bitflash"
echo "   Daemon mode: ./src/bitflash /nogui /gen"
echo "   Pool server starts automatically on Stratum :3333, stats :19012"
echo "==================================================================="
