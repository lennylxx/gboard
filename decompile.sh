#!/usr/bin/env bash
set -euo pipefail

# Optional: decompile Gboard's Java sources with jadx for reverse-engineering.
# Not needed to build GboardIME. Run ./setup.sh first, or pass --apk.
#
# Prerequisites: brew install jadx

usage() {
    cat <<'EOF'
Usage: decompile.sh [--apk PATH]

Options:
  --apk PATH   APK to decompile
               (default: xapk_unpacked/com.google.android.inputmethod.latin.apk)
  -h, --help   Show this help
EOF
}

cd "$(cd "$(dirname "$0")" && pwd)"

APK="xapk_unpacked/com.google.android.inputmethod.latin.apk"
OUT="gboard_apk_source/jadx"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --apk) APK="$2"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Unknown arg: $1"; usage; exit 1 ;;
    esac
done

command -v jadx >/dev/null || { echo "Missing: jadx (brew install jadx)"; exit 1; }
[[ -f "$APK" ]] || { echo "APK not found: $APK (run ./setup.sh first or pass --apk)"; exit 1; }

echo "Decompiling $APK (this may take a few minutes)..."
rm -rf "$OUT"
jadx --quiet --no-res --output-dir "$OUT" "$APK" || true  # some classes fail to decompile (normal for obfuscated APKs)
echo "Decompiled $(find "$OUT" -name '*.java' | wc -l | tr -d ' ') Java files into $OUT/"
