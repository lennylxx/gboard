#!/usr/bin/env bash
set -euo pipefail

cd "$(cd "$(dirname "$0")" && pwd)"

# ── Directory layout ─────────────────────────────────────────────────────────
ENGINE_DIR="GboardIME/engine"
TEST_DIR="tests"
SO_PATH="gboard_apk_source/resources/lib/arm64-v8a/libintegrated_shared_object.so"
PACK_PATH="hmmoemdata/current"

ENGINE_SRCS=(
    "$ENGINE_DIR/hmm_engine.c"
    "$ENGINE_DIR/hmm_native.c"
    "$ENGINE_DIR/hmm_enroll.c"
    "$ENGINE_DIR/hmm_candidates.c"
    "$ENGINE_DIR/hmm_user_dict.c"
    "$ENGINE_DIR/elf_loader.c"
    "$ENGINE_DIR/jni_env.c"
    "$ENGINE_DIR/android_stubs.c"
)

echo "═══ Building test_engine (C) ═══"
clang -g -O0 -DDEBUG=1 -o "$TEST_DIR/test_engine" \
    "$TEST_DIR/test_engine.c" \
    "${ENGINE_SRCS[@]}" \
    -IGboardIME -lpthread

echo "Running test_engine..."
"$TEST_DIR/test_engine" "$SO_PATH" "$PACK_PATH" 2>/dev/null
ENGINE_RC=$?

echo ""
echo "═══ Building test_ime (Swift) ═══"
mkdir -p .swift-module-cache
swiftc -module-cache-path .swift-module-cache -g -parse-as-library -o "$TEST_DIR/test_ime" \
    "$TEST_DIR/test_ime.swift" \
    "GboardIME/ime/PreferencesManager.swift" \
    "GboardIME/ime/PinyinSession.swift" \
    "GboardIME/ime/SessionContextRetriever.swift" \
    "GboardIME/ime/ShiftToggleTracker.swift" \
    "GboardIME/ime/GboardBridge.c" \
    "${ENGINE_SRCS[@]}" \
    -IGboardIME -import-objc-header GboardIME/ime/GboardBridge.h \
    -DDEBUG

echo "Running test_ime..."
"$TEST_DIR/test_ime" "$SO_PATH" "$PACK_PATH" 2>/dev/null
IME_RC=$?

if [ $ENGINE_RC -ne 0 ] || [ $IME_RC -ne 0 ]; then
    exit 1
fi
