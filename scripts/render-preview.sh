#!/usr/bin/env bash
# Renders the README preview image: the real candidate window in light and
# dark appearances. Needs a logged-in GUI session with Screen Recording
# permission for the terminal. Liquid Glass appears on macOS 26 and later.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
OUTPUT="${1:-$ROOT_DIR/preview.png}"
case "$OUTPUT" in /*) ;; *) OUTPUT="$PWD/$OUTPUT" ;; esac

WORK_DIR="$(mktemp -d)"
trap 'rm -rf "$WORK_DIR"' EXIT

IME="$ROOT_DIR/GboardIME/ime"
ENGINE="$ROOT_DIR/GboardIME/engine"
TARGET="arm64-apple-macos13.0"
SDK="$(xcrun --sdk macosx --show-sdk-path)"

objs=()
for src in "$IME"/*.c "$ENGINE"/*.c; do
    obj="$WORK_DIR/$(basename "${src%.c}").o"
    clang -c -Os -target "$TARGET" -isysroot "$SDK" -I"$ENGINE" -I"$IME" -o "$obj" "$src"
    objs+=("$obj")
done

# The app entry point and its InputMethodKit/settings controllers are not
# needed to draw the candidate window (same split as test.sh).
swift_srcs=()
for src in "$IME"/*.swift; do
    case "$(basename "$src")" in
        main.swift|InputController.swift|SettingsWindow.swift) ;;
        *) swift_srcs+=("$src") ;;
    esac
done

swiftc -O -swift-version 5 -parse-as-library -target "$TARGET" -sdk "$SDK" \
    -I"$ENGINE" -I"$IME" -import-objc-header "$IME/GboardBridge.h" \
    -framework Cocoa -framework InputMethodKit \
    -o "$WORK_DIR/render-preview" \
    "$ROOT_DIR/scripts/render_preview.swift" "${swift_srcs[@]}" "${objs[@]}"

"$WORK_DIR/render-preview" "$OUTPUT"
echo "Rendered: $OUTPUT"
