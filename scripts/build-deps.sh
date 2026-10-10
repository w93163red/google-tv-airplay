#!/usr/bin/env bash
# Builds static libcrypto.a for each app ABI into third_party/prebuilt/<abi>.
set -euo pipefail

OPENSSL_VERSION=3.5.9
OPENSSL_SHA256=603f5602e2eef00d77fbd429d34dcd5822bb301757a1bc9cdb24c670f1eb859a
API=30
# Google TV Streamer ships a 32-bit userspace (abilist armeabi-v7a), the emulator is arm64.
ABIS=("arm64-v8a:android-arm64" "armeabi-v7a:android-arm")

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CACHE="$ROOT/third_party/.cache"
ANDROID_HOME="${ANDROID_HOME:-$HOME/Library/Android/sdk}"
export ANDROID_NDK_ROOT="${ANDROID_NDK_ROOT:-$ANDROID_HOME/ndk/27.2.12479018}"

mkdir -p "$CACHE"
TARBALL="$CACHE/openssl-$OPENSSL_VERSION.tar.gz"
if [[ ! -f "$TARBALL" ]]; then
  curl -fL -o "$TARBALL.part" \
    "https://github.com/openssl/openssl/releases/download/openssl-$OPENSSL_VERSION/openssl-$OPENSSL_VERSION.tar.gz"
  mv "$TARBALL.part" "$TARBALL"
fi
echo "$OPENSSL_SHA256  $TARBALL" | shasum -a 256 -c -

HOST_TAG=$(ls "$ANDROID_NDK_ROOT/toolchains/llvm/prebuilt" | head -1)
export PATH="$ANDROID_NDK_ROOT/toolchains/llvm/prebuilt/$HOST_TAG/bin:$PATH"

build_abi() {
  local ABI=$1 TARGET=$2
  local OUT="$ROOT/third_party/prebuilt/$ABI"
  if [[ -f "$OUT/lib/libcrypto.a" && -f "$OUT/include/openssl/opensslv.h" ]] &&
     grep -q "OPENSSL_VERSION_TEXT.*$OPENSSL_VERSION " "$OUT/include/openssl/opensslv.h"; then
    echo "libcrypto $OPENSSL_VERSION already built at $OUT"
    return
  fi

  local SRC="$CACHE/openssl-$OPENSSL_VERSION-$ABI"
  rm -rf "$SRC"
  mkdir -p "$SRC"
  tar -xzf "$TARBALL" -C "$SRC" --strip-components=1

  (
    cd "$SRC"
    ./Configure "$TARGET" -D__ANDROID_API__=$API \
      no-shared no-tests no-apps no-docs no-engine no-ui-console no-module
    make -j"$(sysctl -n hw.ncpu 2>/dev/null || nproc)" build_generated libcrypto.a
  )

  rm -rf "$OUT"
  mkdir -p "$OUT/lib" "$OUT/include"
  cp "$SRC/libcrypto.a" "$OUT/lib/"
  cp -R "$SRC/include/openssl" "$OUT/include/"
  echo "installed libcrypto $OPENSSL_VERSION to $OUT"
}

for entry in "${ABIS[@]}"; do
  build_abi "${entry%%:*}" "${entry#*:}"
done
