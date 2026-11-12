#!/usr/bin/env bash
set -euo pipefail

BASE_URL="https://www.gstatic.com/android/keyboard/hmmpack"
END_DATE=$(date -u "+%Y%m%d")
MAX_REVISION=20
JOBS=50
SEEDS=()

usage() {
    cat <<'EOF'
Usage: find_latest_hmm_manifest.sh --seed URL [--seed URL ...] [OPTIONS]

Finds the newest public Gboard HMM manifest by probing Google's date-based CDN
namespace with parallel HEAD requests from a single curl process, so they
share one connection instead of each opening its own (requires curl 7.66+).

Options:
  --seed URL          Known manifest URL to start from; may be repeated
  --end-date DATE     Last UTC date to check, in YYYYMMDD format
  --max-revision NUM  Highest two-digit revision to check (default: 20)
  --jobs NUM          Concurrent HEAD requests (default: 50)
  -h, --help          Show this help
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --seed) SEEDS+=("$2"); shift 2 ;;
        --end-date) END_DATE="$2"; shift 2 ;;
        --max-revision) MAX_REVISION="$2"; shift 2 ;;
        --jobs) JOBS="$2"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Unknown argument: $1" >&2; usage >&2; exit 2 ;;
    esac
done

if [[ ${#SEEDS[@]} -eq 0 ]]; then
    echo "At least one --seed URL is required." >&2
    exit 2
fi
if [[ ! "$END_DATE" =~ ^[0-9]{8}$ ||
      ! "$MAX_REVISION" =~ ^[0-9]+$ ||
      ! "$JOBS" =~ ^[1-9][0-9]*$ ]]; then
    echo "Invalid date, revision, or job count." >&2
    exit 2
fi

manifest_version() {
    local directory_version
    local filename_version
    directory_version=$(echo "$1" |
        sed -nE 's#^.*/hmmpack/([0-9]{10})/.*$#\1#p')
    filename_version=$(echo "$1" |
        sed -nE 's#^.*/metadata_([0-9]{10})\.json$#\1#p')
    if [[ -n "$directory_version" &&
          "$directory_version" == "$filename_version" ]]; then
        echo "$directory_version"
    fi
}

next_date() {
    local value="$1"
    if date -j -u -f "%Y%m%d" -v+1d "$value" "+%Y%m%d" 2>/dev/null; then
        return
    fi
    date -u -d \
        "${value:0:4}-${value:4:2}-${value:6:2} +1 day" \
        "+%Y%m%d"
}

SEED_URL=""
SEED_VERSION=""
for candidate in "${SEEDS[@]}"; do
    version=$(manifest_version "$candidate")
    if [[ -n "$version" &&
          ( -z "$SEED_VERSION" || "$version" > "$SEED_VERSION" ) ]]; then
        SEED_URL="$candidate"
        SEED_VERSION="$version"
    fi
done

if [[ -z "$SEED_VERSION" ]]; then
    echo "No seed matched the expected gstatic HMM manifest format." >&2
    exit 2
fi

START_DATE=${SEED_VERSION:0:8}
if [[ "$START_DATE" > "$END_DATE" ]]; then
    echo "$SEED_URL"
    exit 0
fi

if ! curl --help all 2>/dev/null | grep -q -- '--parallel-max'; then
    echo "curl 7.66 or newer is required (missing --parallel support)." >&2
    exit 1
fi

TMP_DIR=$(mktemp -d "${TMPDIR:-/tmp}/gboard-hmm-manifest.XXXXXX")
trap 'rm -rf "$TMP_DIR"' EXIT
CANDIDATES="$TMP_DIR/candidates.cfg"
RESULTS="$TMP_DIR/results"

candidate_count=0
date_value="$START_DATE"
while [[ "$date_value" < "$END_DATE" || "$date_value" == "$END_DATE" ]]; do
    revision=0
    while [[ $revision -le $MAX_REVISION ]]; do
        printf -v version "%s%02d" "$date_value" "$revision"
        printf 'url = "%s/%s/metadata_%s.json"\noutput = "/dev/null"\n' \
            "$BASE_URL" "$version" "$version"
        candidate_count=$((candidate_count + 1))
        revision=$((revision + 1))
    done
    date_value=$(next_date "$date_value")
done > "$CANDIDATES"

echo "Checking $candidate_count HMM manifest candidates from $START_DATE to $END_DATE..." >&2

# Unreachable candidates are treated as misses, matching a 404.
curl -sI --parallel --parallel-max "$JOBS" \
    --connect-timeout 5 --max-time 10 \
    -w '%{http_code} %{url_effective}\n' \
    -K "$CANDIDATES" > "$RESULTS" 2>/dev/null || true

LATEST_VERSION=$(sed -nE 's#^200 .*/metadata_([0-9]{10})\.json$#\1#p' "$RESULTS" |
    sort -n | tail -1)
if [[ -z "$LATEST_VERSION" ]]; then
    echo "No newer manifest was reachable; retaining the seed." >&2
    echo "$SEED_URL"
    exit 0
fi

echo "$BASE_URL/$LATEST_VERSION/metadata_$LATEST_VERSION.json"
