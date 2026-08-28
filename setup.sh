#!/usr/bin/env bash
set -euo pipefail

# Extract Gboard XAPK, decompile Java, and download HMM dict pack.
#
# You must supply your own Gboard XAPK file. This project does not
# distribute or automate downloading Google's proprietary binaries.
#
# The dict manifest URL is extracted from decompiled source
# (HmmSuperpacksConfig / defpackage/lap.java → hmm_superpacks_manifest_url)
# which points to a JSON on gstatic.com listing locale packs with download URLs.
#
# Prerequisites: brew install jadx jq curl unzip

usage() {
    cat <<'EOF'
Usage: setup.sh --xapk PATH [OPTIONS]

You must provide your own Gboard XAPK file obtained from your device
or a legitimate source. See README for instructions.

Options:
  --xapk PATH     Path to Gboard XAPK file (required)
  --dict PATH     Use a local dict zip instead of auto-downloading
  --locale CODE   Dict locale to download (default: zh_CN)
                  Available: zh_CN, zh_TW, zh_HK, ko
  -h, --help      Show this help

Examples:
  ./setup.sh --xapk gboard.xapk              # extract + download zh_CN dict
  ./setup.sh --xapk gboard.xapk --locale ko  # extract + download Korean dict
  ./setup.sh --xapk gboard.xapk --dict zh_CN.zip  # use local dict zip
EOF
}

cd "$(cd "$(dirname "$0")" && pwd)"

# ── URLs ──────────────────────────────────────────────────────────────────────
FALLBACK_MANIFEST="https://www.gstatic.com/android/keyboard/hmmpack/2026081806/metadata_2026081806.json"
MANIFEST_SCANNER="./find_latest_hmm_manifest.sh"

# ── Directory layout ─────────────────────────────────────────────────────────
XAPK_DIR="xapk_unpacked"
APK_SOURCE="gboard_apk_source"
RESOURCES="$APK_SOURCE/resources"
JADX_OUT="$APK_SOURCE/jadx"
DICT_DIR="hmmoemdata"
CACHE_FILE="$DICT_DIR/.latest_manifest_url"
SO_FILE="$RESOURCES/lib/arm64-v8a/libintegrated_shared_object.so"

# ── Parse args ───────────────────────────────────────────────────────────────
XAPK_PATH=""
DICT_ZIP=""
LOCALE="zh_CN"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --xapk) XAPK_PATH="$2"; shift 2 ;;
        --dict) DICT_ZIP="$2"; shift 2 ;;
        --locale) LOCALE="$2"; shift 2 ;;
        -h|--help)
            usage; exit 0 ;;
        *) echo "Unknown arg: $1"; exit 1 ;;
    esac
done

if [[ -z "$XAPK_PATH" ]]; then
    echo "Error: --xapk is required. You must supply your own Gboard XAPK file."
    echo ""
    usage
    exit 1
fi

if [[ ! -f "$XAPK_PATH" ]]; then
    echo "Error: XAPK file not found: $XAPK_PATH"
    exit 1
fi

# ── Check prerequisites ─────────────────────────────────────────────────────
missing=()
for cmd in jadx jq unzip curl; do
    command -v "$cmd" >/dev/null || missing+=("$cmd")
done
if [[ ${#missing[@]} -gt 0 ]]; then
    echo "Missing: ${missing[*]}"
    echo "Install with: brew install ${missing[*]}"
    exit 1
fi

# ── Step 1: Extract XAPK (it's a zip of APKs) ──────────────────────────────
echo "Using XAPK: $XAPK_PATH"
echo "Extracting XAPK..."
rm -rf "$XAPK_DIR"
mkdir -p "$XAPK_DIR"
unzip -q -o "$XAPK_PATH" -d "$XAPK_DIR"

# Find the base APK
BASE_APK="$XAPK_DIR/com.google.android.inputmethod.latin.apk"
if [[ ! -f "$BASE_APK" ]]; then
    BASE_APK=$(jq -r '.split_apks[] | select(.id=="base") | .file' "$XAPK_DIR/manifest.json" 2>/dev/null || true)
    if [[ -n "$BASE_APK" ]]; then
        BASE_APK="$XAPK_DIR/$BASE_APK"
    else
        BASE_APK=$(find "$XAPK_DIR" -name "*.apk" -size +10M | head -1)
    fi
fi
echo "Base APK: $BASE_APK"

if [[ -f "$XAPK_DIR/manifest.json" ]]; then
    VERSION=$(jq -r '.version_name // "unknown"' "$XAPK_DIR/manifest.json")
    echo "Gboard version: $VERSION"
fi

# ── Step 2: Extract resources from APK (native libs, assets) ────────────────
echo "Extracting APK resources..."
rm -rf "$RESOURCES"
mkdir -p "$RESOURCES"
unzip -q -o "$BASE_APK" -d "$RESOURCES" \
    "lib/*" "assets/*" "AndroidManifest.xml" "META-INF/*" "res/*" \
    "okhttp3/*" "LICENSE_*" "stamp-*" 2>/dev/null || true

if [[ -f "$SO_FILE" ]]; then
    echo "Found: $SO_FILE ($(du -h "$SO_FILE" | cut -f1))"
else
    echo "WARNING: libintegrated_shared_object.so not found!"
fi

# ── Step 3: Decompile Java sources with jadx ────────────────────────────────
echo "Decompiling Java sources (this may take a few minutes)..."
rm -rf "$JADX_OUT"
jadx --quiet --no-res --output-dir "$JADX_OUT" "$BASE_APK" || true  # some classes fail to decompile (normal for obfuscated APKs)
JAVA_COUNT=$(find "$JADX_OUT" -name "*.java" | wc -l | tr -d ' ')
echo "Decompiled $JAVA_COUNT Java files"

# ── Step 4: Download HMM dict pack from Google's CDN ────────────────────────
# The manifest URL is in the decompiled source (HmmSuperpacksConfig / lap.java).
# It lists available locale packs with direct download URLs.
#
# Manifest URL pattern:
#   https://www.gstatic.com/android/keyboard/hmmpack/<version>/metadata_<version>.json
# Pack URL pattern:
#   https://www.gstatic.com/android/keyboard/hmmpack/<version>/<locale>_<version>.zip

if [[ -n "$DICT_ZIP" ]]; then
    echo "Extracting dict pack from: $DICT_ZIP"
    DICT_NAME=$(basename "$DICT_ZIP" .zip | tr '[:upper:]' '[:lower:]')
    mkdir -p "$DICT_DIR/$DICT_NAME"
    unzip -q -o "$DICT_ZIP" -d "$DICT_DIR/$DICT_NAME"
    echo "Dict pack: $DICT_DIR/$DICT_NAME ($(ls "$DICT_DIR/$DICT_NAME" | wc -l | tr -d ' ') files)"
else
    # Extract manifest URL from decompiled source
    MANIFEST_URL=""
    MANIFEST_FILE=$(grep -rl "hmm_superpacks_manifest_url" "$JADX_OUT" 2>/dev/null | head -1)
    if [[ -n "$MANIFEST_FILE" ]]; then
        MANIFEST_URL=$(grep -o 'https://[^"]*metadata[^"]*\.json' "$MANIFEST_FILE" | head -1)
    fi
    if [[ -z "$MANIFEST_URL" ]]; then
        MANIFEST_URL="$FALLBACK_MANIFEST"
        echo "Using fallback manifest URL (source extraction failed)"
    fi

    SEED_ARGS=(--seed "$FALLBACK_MANIFEST" --seed "$MANIFEST_URL")
    if [[ -f "$CACHE_FILE" ]]; then
        SEED_ARGS+=(--seed "$(cat "$CACHE_FILE")")
    fi
    MANIFEST_URL=$(bash "$MANIFEST_SCANNER" "${SEED_ARGS[@]}")
    mkdir -p "$DICT_DIR"
    printf "%s\n" "$MANIFEST_URL" > "$CACHE_FILE"

    echo "Fetching HMM pack manifest: $MANIFEST_URL"
    MANIFEST_JSON=$(curl -fsSL "$MANIFEST_URL")

    echo "Available packs:"
    echo "$MANIFEST_JSON" | jq -r '.packs[] | "  \(.locale)\t\(.name)\t\(.compressed_size) bytes"'

    PACK_URL=$(echo "$MANIFEST_JSON" | jq -r ".packs[] | select(.locale==\"$LOCALE\") | .download_urls[0]")
    PACK_NAME=$(echo "$MANIFEST_JSON" | jq -r ".packs[] | select(.locale==\"$LOCALE\") | .name")

    if [[ -z "$PACK_URL" || "$PACK_URL" == "null" ]]; then
        echo "ERROR: No pack found for locale '$LOCALE'"
        echo "Available locales: $(echo "$MANIFEST_JSON" | jq -r '.packs[].locale' | tr '\n' ' ')"
        exit 1
    fi

    DICT_NAME=$(echo "$PACK_NAME" | tr '[:upper:]' '[:lower:]')
    ZIP_FILE="${PACK_NAME}.zip"

    if [[ -d "$DICT_DIR/$DICT_NAME" ]]; then
        echo "HMM pack is current: $DICT_DIR/$DICT_NAME"
    else
        echo "Downloading $PACK_NAME..."
        curl -fL -o "$ZIP_FILE" "$PACK_URL"
        echo "Downloaded: $ZIP_FILE ($(du -h "$ZIP_FILE" | cut -f1))"

        mkdir -p "$DICT_DIR/$DICT_NAME"
        unzip -q -o "$ZIP_FILE" -d "$DICT_DIR/$DICT_NAME"
        FILE_COUNT=$(ls "$DICT_DIR/$DICT_NAME" | wc -l | tr -d ' ')
        echo "Extracted: $DICT_DIR/$DICT_NAME/ ($FILE_COUNT files)"
    fi
fi

if [[ -e "$DICT_DIR/current" && ! -L "$DICT_DIR/current" ]]; then
    echo "ERROR: $DICT_DIR/current exists and is not a symbolic link"
    exit 1
fi
ln -sfn "$DICT_NAME" "$DICT_DIR/current"

# ── Summary ──────────────────────────────────────────────────────────────────
echo ""
echo "Setup complete."
echo "  XAPK:       $XAPK_PATH"
echo "  Resources:  $RESOURCES/"
echo "  Sources:    $JADX_OUT/ ($JAVA_COUNT files)"
if [[ -L "$DICT_DIR/current" ]]; then
    echo "  Dict pack:  $DICT_DIR/$(readlink "$DICT_DIR/current")/"
fi
echo ""
echo "Next: cd GboardIME && bash build.sh install"
