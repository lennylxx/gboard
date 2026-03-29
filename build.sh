#!/usr/bin/env bash
set -euo pipefail

cd "$(cd "$(dirname "$0")" && pwd)"

# ── Directory layout ─────────────────────────────────────────────────────────
XCODE_DIR="GboardIME"
BUILD_DIR="$XCODE_DIR/build/release"
APP_NAME="GboardIME.app"
INSTALL_DIR="$HOME/Library/Input Methods"
ENTITLEMENTS="$XCODE_DIR/GboardIME.entitlements"
SO_SRC="gboard_apk_source/resources/lib/arm64-v8a/libintegrated_shared_object.so"
PACK_SRC="hmmoemdata/zh_cn_2025090307"

usage() {
    echo "Usage: $0 [build|install|uninstall|clean]"
    echo "  build     Build the app (default)"
    echo "  install   Build, install to ~/Library/Input Methods, and sign"
    echo "  uninstall Remove from ~/Library/Input Methods"
    echo "  clean     Remove build artifacts"
}

copy_engine_resources() {
    local resources="$1/Contents/Resources"
    mkdir -p "$resources"

    if [ -f "$SO_SRC" ]; then
        cp "$SO_SRC" "$resources/libintegrated_shared_object.so"
        echo "Copied .so"
    else
        echo "WARNING: $SO_SRC not found — offline engine won't work"
    fi

    if [ -d "$PACK_SRC" ]; then
        mkdir -p "$resources/hmmoemdata"
        cp -R "$PACK_SRC" "$resources/hmmoemdata/zh_cn_2025090307"
        echo "Copied pinyin data pack"
    else
        echo "WARNING: $PACK_SRC not found — offline engine won't work"
    fi
}

do_build() {
    echo "Building GboardIME (Release)..."
    cd "$XCODE_DIR"
    xcodebuild -quiet \
        -scheme GboardIME \
        -configuration Release \
        build \
        CONFIGURATION_BUILD_DIR="build/release"
    cd ..
    copy_engine_resources "$BUILD_DIR/$APP_NAME"
    echo "Built: $BUILD_DIR/$APP_NAME"
}

do_install() {
    do_build

    echo "Installing to $INSTALL_DIR..."
    killall GboardIME 2>/dev/null || true
    sleep 2

    rm -rf "$INSTALL_DIR/$APP_NAME"
    cp -R "$BUILD_DIR/$APP_NAME" "$INSTALL_DIR/$APP_NAME"

    codesign --force --deep --sign - \
        --entitlements "$ENTITLEMENTS" \
        "$INSTALL_DIR/$APP_NAME"

    echo "Installed and signed."
    echo ""
    echo "To activate:"
    echo "  1. Log out and log back in (or restart)"
    echo "  2. System Settings → Keyboard → Input Sources → Edit → + → Chinese Simplified → Gboard"
    echo "  3. Switch to Gboard from the menu bar input source picker"
}

do_uninstall() {
    killall GboardIME 2>/dev/null || true
    rm -rf "$INSTALL_DIR/$APP_NAME"
    echo "Uninstalled."
}

do_clean() {
    rm -rf "$BUILD_DIR"
    rm -f tests/test_engine
    rm -rf tests/test_engine.dSYM
    echo "Cleaned."
}

case "${1:-build}" in
    build)     do_build ;;
    install)   do_install ;;
    uninstall) do_uninstall ;;
    clean)     do_clean ;;
    -h|--help) usage ;;
    *)         echo "Unknown command: $1"; usage; exit 1 ;;
esac
