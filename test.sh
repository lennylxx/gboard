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
    "$ENGINE_DIR/linux_abi.c"
)

echo "═══ Building test_linux_abi (C) ═══"
clang -g -O0 -DDEBUG=1 -o "$TEST_DIR/test_linux_abi" \
    "$TEST_DIR/test_linux_abi.c" \
    "$ENGINE_DIR/android_stubs.c" \
    "$ENGINE_DIR/linux_abi.c" \
    "$ENGINE_DIR/elf_loader.c" \
    -IGboardIME -lpthread

echo "Running test_linux_abi..."
"$TEST_DIR/test_linux_abi"

echo ""
echo "═══ Checking constructor probe generation ═══"
PROBE_DIR="$(mktemp -d)"
trap 'rm -rf "$PROBE_DIR"' EXIT
clang -O2 -o "$PROBE_DIR/gen-ctor-probe" scripts/gen-ctor-probe.c \
    "$ENGINE_DIR/elf_loader.c" \
    "$ENGINE_DIR/android_stubs.c" \
    "$ENGINE_DIR/linux_abi.c" \
    -IGboardIME -lpthread
# A planted symlink at the output path must be replaced, not followed.
ln -s "$PWD/$SO_PATH" "$PROBE_DIR/lib.so"
echo keep > "$PROBE_DIR/victim"
ln -s "$PROBE_DIR/victim" "$PROBE_DIR/lib.so.ctorprobe"
if ! "$PROBE_DIR/gen-ctor-probe" "$PROBE_DIR/lib.so" "$PACK_PATH" >/dev/null; then
    echo "FAIL: constructor probe generator exited with an error"
    exit 1
fi
if [ "$(cat "$PROBE_DIR/victim")" != keep ] ||
   [ -L "$PROBE_DIR/lib.so.ctorprobe" ] ||
   [ "$(head -c 8 "$PROBE_DIR/lib.so.ctorprobe")" != GBCTOR02 ]; then
    echo "FAIL: constructor probe generation"
    exit 1
fi
echo "PASS: constructor probe replaces symlinks atomically"
if ! "$PROBE_DIR/gen-ctor-probe" --verify "$PROBE_DIR/lib.so" "$PACK_PATH" >/dev/null; then
    echo "FAIL: elf_load ignored a freshly generated constructor probe"
    exit 1
fi
# Flip a digest byte (header: 8-byte magic, size, count, then the digest).
b=$(od -An -tu1 -j40 -N1 "$PROBE_DIR/lib.so.ctorprobe" | tr -d ' ')
printf "\\$(printf %03o $((255 - b)))" |
    dd of="$PROBE_DIR/lib.so.ctorprobe" bs=1 seek=40 conv=notrunc 2>/dev/null
if "$PROBE_DIR/gen-ctor-probe" --verify "$PROBE_DIR/lib.so" "$PACK_PATH" >/dev/null 2>&1; then
    echo "FAIL: elf_load accepted a constructor probe with a wrong digest"
    exit 1
fi
echo "PASS: elf_load uses matching probes and rejects stale ones"

echo ""
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
swiftc -g -parse-as-library -o "$TEST_DIR/test_ime" \
    "$TEST_DIR/test_ime.swift" \
    "$TEST_DIR/test_preferences.swift" \
    "GboardIME/ime/PinyinSession.swift" \
    "GboardIME/ime/PinyinReading.swift" \
    "GboardIME/ime/Preferences.swift" \
    "GboardIME/ime/UserDictionary.swift" \
    "GboardIME/ime/LearnedWords.swift" \
    "GboardIME/ime/Shortcuts.swift" \
    "GboardIME/ime/ShiftToggleTracker.swift" \
    "GboardIME/ime/SymbolPanel.swift" \
    "GboardIME/ime/SessionContextRetriever.swift" \
    "GboardIME/ime/CandidateWindow.swift" \
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
