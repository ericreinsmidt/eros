#!/bin/sh
# Build EROS's minarch from NextUI source with EROS overrides applied.
#
# minarch is the libretro host EROS launches per game (prompt: "built from
# MinUI/NextUI source, bundled with EROS"). We don't fork NextUI: this copies
# a pristine workspace, overlays only the files in overrides/, and builds in
# the tg5040 toolchain container. Output: vendor/minarch.elf (+ its runtime
# libs already come from mk/fetch-vendor.sh).
#
# Usage: minarch/build.sh
#   NEXTUI_SRC=/path/to/NextUI/workspace   (default ~/Projects/NextUI/workspace)

set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
NEXTUI_SRC=${NEXTUI_SRC:-$HOME/Projects/NextUI/workspace}
IMAGE=ghcr.io/loveretro/tg5040-toolchain:latest
BUILD=${EROS_MINARCH_BUILD:-/tmp/eros-minarch/workspace}

[ -d "$NEXTUI_SRC/all/minarch" ] || { echo "NextUI workspace not found at $NEXTUI_SRC"; exit 1; }

echo "copying workspace -> $BUILD"
rm -rf "$BUILD"
mkdir -p "$BUILD"
cp -R "$NEXTUI_SRC/." "$BUILD/"

echo "applying EROS overrides"
if [ -d "$ROOT/minarch/overrides" ]; then
	( cd "$ROOT/minarch/overrides" && cp -R . "$BUILD/" )
fi

echo "building in container"
docker run --rm -v "$BUILD":/root/workspace "$IMAGE" /bin/bash -c '
	set -e
	source ~/.bashrc 2>/dev/null || true
	cd /root/workspace/tg5040/libmsettings && make
	cd /root/workspace/all/minarch && make PLATFORM=tg5040
'

OUT="$BUILD/all/minarch/build/tg5040/minarch.elf"
[ -f "$OUT" ] || { echo "build failed: no minarch.elf"; exit 1; }
mkdir -p "$ROOT/vendor"
cp "$OUT" "$ROOT/vendor/minarch.elf"
echo "vendor/minarch.elf updated ($(cd "$ROOT" && ls -la vendor/minarch.elf | awk '{print $5}') bytes)"
