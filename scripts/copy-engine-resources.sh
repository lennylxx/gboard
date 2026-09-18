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
