#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -ne 1 ]; then
    echo "Usage: $0 /path/to/GboardIME.app" >&2
    exit 2
fi

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
APP_BUNDLE="$1"
RESOURCES="$APP_BUNDLE/Contents/Resources"
SO_SOURCE="$ROOT_DIR/gboard_apk_source/resources/lib/arm64-v8a/libintegrated_shared_object.so"
PACK_SOURCE="$(cd "$ROOT_DIR/hmmoemdata/current" && pwd -P)"
PACK_DESTINATION="$RESOURCES/hmmoemdata/current"

if [ ! -f "$SO_SOURCE" ]; then
    echo "Missing native engine: $SO_SOURCE" >&2
    exit 1
fi

mkdir -p "$RESOURCES"
rm -rf "$PACK_DESTINATION"
mkdir -p "$PACK_DESTINATION"
cp -f "$SO_SOURCE" "$RESOURCES/libintegrated_shared_object.so"
cp -R "$PACK_SOURCE/." "$PACK_DESTINATION/"

# Probe the native constructors once at build time so the IME can skip the
# fork probe at launch. The loader verifies the result matches the library.
ENGINE_DIR="$ROOT_DIR/GboardIME/engine"
PROBE_TOOL_DIR="$(mktemp -d)"
trap 'rm -rf "$PROBE_TOOL_DIR"' EXIT
rm -f "$RESOURCES"/libintegrated_shared_object.so.ctorprobe*
xcrun clang -O2 -arch arm64 -I"$ROOT_DIR/GboardIME" \
    -o "$PROBE_TOOL_DIR/gen_ctor_probe" \
    "$ROOT_DIR/scripts/gen_ctor_probe.c" \
    "$ENGINE_DIR/elf_loader.c" \
    "$ENGINE_DIR/android_stubs.c" \
    "$ENGINE_DIR/linux_abi.c" \
    -lpthread
"$PROBE_TOOL_DIR/gen_ctor_probe" \
    "$RESOURCES/libintegrated_shared_object.so" "$PACK_DESTINATION" >/dev/null
