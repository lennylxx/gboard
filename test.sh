#!/usr/bin/env bash
set -euo pipefail

cd "$(cd "$(dirname "$0")" && pwd)"

# ── Directory layout ─────────────────────────────────────────────────────────
ENGINE_DIR="GboardIME/engine"
TEST_DIR="tests"
SO_PATH="gboard_apk_source/resources/lib/arm64-v8a/libintegrated_shared_object.so"
PACK_PATH="hmmoemdata/zh_cn_2025090307"

echo "Building test_engine..."
clang -g -O0 -DDEBUG=1 -o "$TEST_DIR/test_engine" \
    "$TEST_DIR/test_engine.c" \
    "$ENGINE_DIR/hmm_engine.c" \
    "$ENGINE_DIR/hmm_native.c" \
    "$ENGINE_DIR/hmm_enroll.c" \
    "$ENGINE_DIR/hmm_candidates.c" \
    "$ENGINE_DIR/elf_loader.c" \
    "$ENGINE_DIR/jni_env.c" \
    "$ENGINE_DIR/android_stubs.c" \
    -IGboardIME -lpthread

echo "Running tests..."
"$TEST_DIR/test_engine" "$SO_PATH" "$PACK_PATH" 2>/dev/null
