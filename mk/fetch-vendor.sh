#!/bin/sh
# Populate vendor/ with the prebuilt tg5040 runtime EROS bundles:
# minarch.elf + its runtime libs + the 7 cores. Everything comes from a
# NextUI release (GPL-3.0; cores carry their own licenses -- snes9x and
# picodrive are non-commercial). See THIRD-PARTY-LICENSES.md.
#
# Usage: mk/fetch-vendor.sh [download-dir]
# Reuses NextUI-*-base.zip / NextUI-*-extras.zip in download-dir if present,
# otherwise downloads them with gh from LoveRetro/NextUI.

set -e
REL=v6.11.2
DL=${1:-/tmp/eros-vendor}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
VENDOR=$ROOT/vendor

mkdir -p "$DL" "$VENDOR/cores" "$VENDOR/lib"

cd "$DL"
BASE=$(ls NextUI-*-base.zip 2> /dev/null | head -1 || true)
EXTRAS=$(ls NextUI-*-extras.zip 2> /dev/null | head -1 || true)
if [ -z "$BASE" ]; then
	gh release download $REL -R LoveRetro/NextUI -p 'NextUI-*-base.zip'
	BASE=$(ls NextUI-*-base.zip | head -1)
fi
if [ -z "$EXTRAS" ]; then
	gh release download $REL -R LoveRetro/NextUI -p 'NextUI-*-extras.zip'
	EXTRAS=$(ls NextUI-*-extras.zip | head -1)
fi

# base zip nests the system payload as MinUI.zip
unzip -o -j "$BASE" MinUI.zip -d "$DL" > /dev/null

# minarch.elf itself is built from source by minarch/build.sh (EROS-branded,
# trimmed menu). Here we only take its runtime libs + the cores.
unzip -o -j "$DL/MinUI.zip" \
	'.system/tg5040/lib/*' \
	-d "$VENDOR/lib" > /dev/null
unzip -o -j "$DL/MinUI.zip" \
	.system/tg5040/cores/fceumm_libretro.so \
	.system/tg5040/cores/snes9x_libretro.so \
	.system/tg5040/cores/picodrive_libretro.so \
	.system/tg5040/cores/pcsx_rearmed_libretro.so \
	.system/tg5040/cores/gambatte_libretro.so \
	-d "$VENDOR/cores" > /dev/null

unzip -o -j "$EXTRAS" \
	Emus/tg5040/MGBA.pak/mgba_libretro.so \
	Emus/tg5040/PCE.pak/mednafen_pce_fast_libretro.so \
	-d "$VENDOR/cores" > /dev/null

# minarch hardcodes /.system/res (fonts) and /.system/tg5040/shaders at
# compile time and crashes without them; governor.sh is called by name
mkdir -p "$VENDOR/system/res" "$VENDOR/system/tg5040/shaders" "$VENDOR/bin"
unzip -o -j "$DL/MinUI.zip" '.system/res/*' -d "$VENDOR/system/res" > /dev/null
unzip -o -j "$DL/MinUI.zip" '.system/tg5040/shaders/*' -d "$VENDOR/system/tg5040/shaders" > /dev/null
unzip -o -j "$DL/MinUI.zip" .system/tg5040/bin/governor.sh -d "$VENDOR/bin" > /dev/null

chmod +x "$VENDOR/minarch.elf"
echo "vendor/ ready:"
ls "$VENDOR" "$VENDOR/cores" "$VENDOR/lib"
