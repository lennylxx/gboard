#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"

ENGINE_DIR="../GboardIME/engine"

echo "Building test_engine..."
clang -g -O0 -o test_engine \
    test_engine.c \
    "$ENGINE_DIR/hmm_engine.c" \
    "$ENGINE_DIR/hmm_native.c" \
    "$ENGINE_DIR/hmm_enroll.c" \
    "$ENGINE_DIR/hmm_candidates.c" \
    "$ENGINE_DIR/elf_loader.c" \
    "$ENGINE_DIR/jni_env.c" \
    "$ENGINE_DIR/android_stubs.c" \
    -I../GboardIME -lpthread

echo "Running tests..."
./test_engine \
    "../source/resources/lib/arm64-v8a/libintegrated_shared_object.so" \
    "../hmmoemdata/zh_cn_2025090307" \
    2>/dev/null
