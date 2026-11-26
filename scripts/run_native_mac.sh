#!/usr/bin/env bash
set -euo pipefail
REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"
export PATH="/opt/homebrew/bin:/usr/local/bin:$PATH"
# fluidsynth.pc lists readline as a private requirement; Homebrew's readline is keg-only.
export PKG_CONFIG_PATH="/opt/homebrew/opt/readline/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
BUILD_DIR="$REPO_DIR/build/mac-native"
cmake -S "$REPO_DIR" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release \
  -DRICH4_SANITIZERS=OFF -DRICH4_BUILD_VIEWER=OFF -DRICH4_BUILD_GAME=ON -DBUILD_TESTING=OFF
cmake --build "$BUILD_DIR" --target rich4-native --parallel
mkdir -p "$REPO_DIR/artifacts"
exec "$BUILD_DIR/rich4-native.app/Contents/MacOS/rich4-native" \
  --assets "${RICH4_ASSETS:-$REPO_DIR/game-assets}" "$@" \
  > "$REPO_DIR/artifacts/native-runtime.log" 2>&1
