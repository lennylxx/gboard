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
clang -O2 -o "$PROBE_DIR/gen_ctor_probe" scripts/gen_ctor_probe.c \
    "$ENGINE_DIR/elf_loader.c" \
    "$ENGINE_DIR/android_stubs.c" \
    "$ENGINE_DIR/linux_abi.c" \
    -IGboardIME -lpthread
# A planted symlink at the output path must be replaced, not followed.
ln -s "$PWD/$SO_PATH" "$PROBE_DIR/lib.so"
echo keep > "$PROBE_DIR/victim"
ln -s "$PROBE_DIR/victim" "$PROBE_DIR/lib.so.ctorprobe"
if ! "$PROBE_DIR/gen_ctor_probe" "$PROBE_DIR/lib.so" "$PACK_PATH" >/dev/null; then
    echo "FAIL: constructor probe generator exited with an error"
    exit 1
fi
if [ "$(cat "$PROBE_DIR/victim")" != keep ] ||
   [ -L "$PROBE_DIR/lib.so.ctorprobe" ] ||
   [ "$(head -c 8 "$PROBE_DIR/lib.so.ctorprobe")" != GBCTOR03 ]; then
    echo "FAIL: constructor probe generation"
    exit 1
fi
echo "PASS: constructor probe replaces symlinks atomically"
if ! "$PROBE_DIR/gen_ctor_probe" --verify "$PROBE_DIR/lib.so" "$PACK_PATH" >/dev/null; then
    echo "FAIL: elf_load ignored a freshly generated constructor probe"
    exit 1
fi
# Point the first TPIDR site at the ELF header, outside the code. The sites
# follow the 56-byte header, one byte per constructor, and a site count.
cp "$PROBE_DIR/lib.so.ctorprobe" "$PROBE_DIR/good.ctorprobe"
ctors=$(od -An -tu8 -j16 -N8 "$PROBE_DIR/lib.so.ctorprobe" | tr -d ' ')
dd if=/dev/zero of="$PROBE_DIR/lib.so.ctorprobe" bs=1 count=4 \
    seek=$((56 + ctors + 8)) conv=notrunc 2>/dev/null
if "$PROBE_DIR/gen_ctor_probe" --verify "$PROBE_DIR/lib.so" "$PACK_PATH" >/dev/null 2>&1; then
    echo "FAIL: elf_load accepted a probe with an invalid TPIDR site"
    exit 1
fi
mv "$PROBE_DIR/good.ctorprobe" "$PROBE_DIR/lib.so.ctorprobe"
# Flip a digest byte (header: 8-byte magic, size, count, then the digest).
b=$(od -An -tu1 -j40 -N1 "$PROBE_DIR/lib.so.ctorprobe" | tr -d ' ')
printf "\\$(printf %03o $((255 - b)))" |
    dd of="$PROBE_DIR/lib.so.ctorprobe" bs=1 seek=40 conv=notrunc 2>/dev/null
if "$PROBE_DIR/gen_ctor_probe" --verify "$PROBE_DIR/lib.so" "$PACK_PATH" >/dev/null 2>&1; then
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
# The generator rejects tables the matcher could not use.
mkdir -p "$PROBE_DIR/bad"
printf '木\tmu\n' > "$PROBE_DIR/bad/chaizi_components.tsv"
printf '林\t木木\n森\t木木木\n' > "$PROBE_DIR/bad/chaizi_characters.tsv"
if ! python3 scripts/gen_chaizi_tables.py "$PROBE_DIR/bad" "$PROBE_DIR/bad"; then
    echo "FAIL: chaizi generator rejected a valid table"
    exit 1
fi
# Each invalid table must fail with the generator's own message.
for bad in '林\t木口\n|no reading for 口' '林\t木木\n林\t木木\n|duplicate character 林' \
           '林\t林木\n|bad character line'; do
    printf "${bad%%|*}" > "$PROBE_DIR/bad/chaizi_characters.tsv"
    if python3 scripts/gen_chaizi_tables.py "$PROBE_DIR/bad" "$PROBE_DIR/bad" \
        2> "$PROBE_DIR/bad/stderr" ||
       ! grep -q "gen_chaizi_tables: .*${bad#*|}" "$PROBE_DIR/bad/stderr"; then
        echo "FAIL: chaizi generator did not reject: ${bad#*|}"
        exit 1
    fi
done
echo "PASS: chaizi generator validates tables"
python3 scripts/gen_chaizi_tables.py "$PROBE_DIR"
clang -c -g -o "$TEST_DIR/ChaiziData.o" GboardIME/ime/ChaiziData.c \
    -IGboardIME/ime -I"$PROBE_DIR"
swiftc -g -parse-as-library -o "$TEST_DIR/test_ime" \
    "$TEST_DIR/test_ime.swift" \
    "$TEST_DIR/test_preferences.swift" \
    "GboardIME/ime/PinyinSession.swift" \
    "GboardIME/ime/PinyinReading.swift" \
    "GboardIME/ime/Preferences.swift" \
    "GboardIME/ime/UserDictionary.swift" \
    "GboardIME/ime/LearnedWords.swift" \
    "GboardIME/ime/Shortcuts.swift" \
    "GboardIME/ime/ChaiziDictionary.swift" \
    "GboardIME/ime/ShiftToggleTracker.swift" \
    "GboardIME/ime/SymbolPanel.swift" \
    "GboardIME/ime/SessionContextRetriever.swift" \
    "GboardIME/ime/CandidateWindow.swift" \
    "GboardIME/ime/GboardBridge.c" \
    "$TEST_DIR/ChaiziData.o" \
    "${ENGINE_SRCS[@]}" \
    -IGboardIME -import-objc-header GboardIME/ime/GboardBridge.h \
    -DDEBUG

echo "Running test_ime..."
"$TEST_DIR/test_ime" "$SO_PATH" "$PACK_PATH" 2>/dev/null
IME_RC=$?

if [ $ENGINE_RC -ne 0 ] || [ $IME_RC -ne 0 ]; then
    exit 1
fi
