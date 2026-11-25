#!/usr/bin/env bash
# Build one backend for RADIO against an EdgeTX checkout and install it,
# with the dispatcher, into DIST (default: dist/).
#
# usage: scripts/build.sh RADIO [EDGETX_SRC] [DIST]
#   RADIO       tx16s | pocket
#   EDGETX_SRC  EdgeTX checkout (default: third_party/edgetx)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
RADIO="${1:?usage: $0 RADIO [EDGETX_SRC] [DIST]}"
SRC="$(cd "${2:-$ROOT/third_party/edgetx}" && pwd)"
DIST="${3:-$ROOT/dist}"

VER=$(sed -nE 's/^set\(VERSION_(MAJOR|MINOR|REVISION) "([0-9]+)"\)/\2/p' "$SRC/CMakeLists.txt" | paste -sd.)
TAG="v$VER"
BUILD="$ROOT/build/$RADIO-$TAG"

cmake -S "$ROOT" -B "$BUILD" -G Ninja -Wno-dev \
  -DLINT_RADIO="$RADIO" -DEDGETX_SRC="$SRC" -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD" --target edgetx-lint-backend edgetx-lint

mkdir -p "$DIST/bin" "$DIST/libexec/edgetx-lint"
install -m 755 "$BUILD/bin/edgetx-lint" "$DIST/bin/edgetx-lint"
install -m 755 "$BUILD/libexec/edgetx-lint/edgetx-lint-$RADIO-$TAG" "$DIST/libexec/edgetx-lint/"
strip "$DIST/libexec/edgetx-lint/edgetx-lint-$RADIO-$TAG" || true
echo "installed $DIST/libexec/edgetx-lint/edgetx-lint-$RADIO-$TAG"
