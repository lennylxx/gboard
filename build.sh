#!/usr/bin/env bash
set -euo pipefail

cd "$(cd "$(dirname "$0")" && pwd)"

# ── Directory layout ─────────────────────────────────────────────────────────
XCODE_DIR="GboardIME"
BUILD_DIR="$XCODE_DIR/build/release"
APP_NAME="GboardIME.app"
INSTALL_DIR="$HOME/Library/Input Methods"
ENTITLEMENTS="$XCODE_DIR/GboardIME.entitlements"

usage() {
    echo "Usage: $0 [build|install|uninstall|clean]"
    echo "  build     Build the app (default)"
    echo "  install   Build, install to ~/Library/Input Methods, and sign"
    echo "  uninstall Remove from ~/Library/Input Methods"
    echo "  clean     Remove build artifacts"
}

copy_engine_resources() {
    scripts/copy-engine-resources.sh "$1"
    echo "Copied native engine and pinyin data pack"
}

do_build() {
    echo "Building GboardIME (Release)..."
    cd "$XCODE_DIR"
    xcodebuild -quiet \
        -scheme GboardIME \
        -destination 'platform=macOS' \
        -derivedDataPath "build/DerivedData" \
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

    # Force macOS LaunchServices to register the new Input Method immediately
    local lsregister="/System/Library/Frameworks/CoreServices.framework/Versions/Current/Frameworks/LaunchServices.framework/Versions/Current/Support/lsregister"
    if [ -x "$lsregister" ]; then
        "$lsregister" -R -f "$INSTALL_DIR/$APP_NAME" 2>/dev/null || true
    fi

    echo ""
    echo "To activate:"
    echo "  1. Click the '+' button at the bottom-left of the Input Sources window"
    echo "  2. Select 'Chinese, Simplified' on the left"
    echo "  3. Find and select 'Gboard' on the right, then click 'Add'"
    echo "  4. Switch to Gboard from the menu bar input source picker"
}

do_uninstall() {
    killall GboardIME 2>/dev/null || true
    rm -rf "$INSTALL_DIR/$APP_NAME"
    echo "Uninstalled."
}

do_clean() {
    rm -rf "$XCODE_DIR/build"
    rm -f tests/test_engine tests/test_ime
    rm -rf tests/test_engine.dSYM tests/test_ime.dSYM .swift-module-cache
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
