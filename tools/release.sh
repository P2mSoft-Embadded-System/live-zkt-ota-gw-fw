#!/usr/bin/env bash
# Build a release image and publish it for OTA.
#
#   tools/release.sh <version> [blink_ms] [--bad] [--push]
#
#   --bad   build an image that crashes on boot (to test rollback)
#   --push  git push after committing (devices see it within ~5 min, GitHub CDN cache)
#
# Produces firmware/firmware-v<version>.bin and appends it to versions.txt.
set -euo pipefail
cd "$(dirname "$0")/.."

VERSION="" BLINK=1000 BAD=0 PUSH=0
for arg in "$@"; do
  case "$arg" in
    --bad)  BAD=1 ;;
    --push) PUSH=1 ;;
    *) if [ -z "$VERSION" ]; then VERSION="$arg"; else BLINK="$arg"; fi ;;
  esac
done

if ! [[ "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
  echo "usage: $0 <major.minor.patch> [blink_ms] [--bad] [--push]" >&2
  exit 1
fi
if grep -qE "^${VERSION//./\\.}[[:space:]]" versions.txt 2>/dev/null; then
  echo "version $VERSION is already listed in versions.txt" >&2
  exit 1
fi

FW_VERSION="$VERSION" BLINK_MS="$BLINK" SIMULATE_BAD="$BAD" pio run -e esp32dev

FILE="firmware/firmware-v${VERSION}.bin"
mkdir -p firmware
cp .pio/build/esp32dev/firmware.bin "$FILE"
SIZE=$(stat -c %s "$FILE")
SHA=$(sha256sum "$FILE" | cut -d' ' -f1)
printf '%s %s %s %s\n' "$VERSION" "$FILE" "$SIZE" "$SHA" >> versions.txt

git add "$FILE" versions.txt
MSG="release v${VERSION} (blink ${BLINK} ms)"
[ "$BAD" = 1 ] && MSG="$MSG [deliberately broken: rollback test]"
git commit -q -m "$MSG"
echo "released $VERSION: $FILE  $SIZE bytes  sha256 $SHA"

if [ "$PUSH" = 1 ]; then
  git push origin HEAD
fi
