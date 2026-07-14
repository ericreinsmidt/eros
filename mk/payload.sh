#!/bin/sh
# Assemble the installable SD payload under out/sd/.
# Copy the contents of out/sd/ to the root of a FAT32 SD card, insert into
# a stock Brick, power on: first boot installs the runtrimui.sh hook and
# every boot after that goes straight to EROS.

set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=$ROOT/out/sd

[ -f "$ROOT/build/eros.elf" ] || { echo "run make first"; exit 1; }
[ -f "$ROOT/vendor/minarch.elf" ] || { echo "run mk/fetch-vendor.sh first"; exit 1; }

rm -rf "$OUT"
mkdir -p "$OUT/eros/icons" "$OUT/eros/cores" "$OUT/eros/lib" "$OUT/eros/bin" \
         "$OUT/.tmp_update" "$OUT/trimui/app" "$OUT/Roms" "$OUT/Bios" "$OUT/Saves" \
         "$OUT/.system/res" "$OUT/.system/tg5040/shaders"

cp "$ROOT/build/eros.elf" "$OUT/eros/"
[ -f "$ROOT/build/muse" ] && cp "$ROOT/build/muse" "$OUT/eros/"  # Muse music player app
cp "$ROOT/sd/eros/launch.sh" "$OUT/eros/"
cp "$ROOT/sd/eros/btout.sh" "$OUT/eros/"   # game-audio-over-BT forwarder (aplay)
chmod +x "$OUT/eros/btout.sh"
cp "$ROOT/THIRD-PARTY-LICENSES.md" "$OUT/eros/"   # notices for redistributed vendor/ software
cp "$ROOT/config/systems.cfg" "$ROOT/config/eros.cfg" "$OUT/eros/"
cp "$ROOT/res/icons/"*.png "$OUT/eros/icons/"
cp "$ROOT/res/fonts/menu.ttf" "$OUT/eros/"           # in-game menu font (OFL)
cp "$ROOT/res/logo/EROS-boot-animation.mp4" "$OUT/eros/EROS-boot.mp4"  # boot animation
cp "$ROOT/res/boot/bootlogo.bmp" "$OUT/eros/"        # u-boot splash (applied on first boot)
cp "$ROOT/res/logo/EROS-boot-black.png" "$OUT/eros/splash.png"  # pic2fb loading splash: "ER S" (frame 0 of the boot animation, so the O just fills in). Applied on first boot.
cp "$ROOT/build/setbright" "$OUT/eros/"              # boot-time brightness helper
chmod +x "$OUT/eros/setbright"
cp "$ROOT/build/swread" "$OUT/eros/"                 # BT audio-switch reader (in-game BT routing)
chmod +x "$OUT/eros/swread"
cp "$ROOT/vendor/minarch.elf" "$OUT/eros/"
cp "$ROOT/vendor/cores/"*.so "$OUT/eros/cores/"
cp "$ROOT/vendor/lib/"* "$OUT/eros/lib/"
# minarch resolves res/shaders at compile-time /.system paths, and calls
# $SYSTEM_PATH/bin/governor.sh; without these it segfaults at startup
cp "$ROOT/vendor/system/res/"* "$OUT/.system/res/"
cp "$ROOT/vendor/system/tg5040/shaders/"* "$OUT/.system/tg5040/shaders/"
cp "$ROOT/vendor/bin/governor.sh" "$OUT/eros/bin/"
chmod +x "$OUT/eros/bin/governor.sh"
cp "$ROOT/sd/.tmp_update/updater" "$ROOT/sd/.tmp_update/tg5040.sh" "$OUT/.tmp_update/"
cp "$ROOT/sd/trimui/app/MainUI" "$ROOT/sd/trimui/app/runtrimui.sh" "$OUT/trimui/app/"
chmod +x "$OUT/.tmp_update/updater" "$OUT/.tmp_update/tg5040.sh" \
         "$OUT/eros/launch.sh" "$OUT/eros/eros.elf" "$OUT/eros/minarch.elf" \
         "$OUT/trimui/app/MainUI" "$OUT/trimui/app/runtrimui.sh"
[ -f "$OUT/eros/muse" ] && chmod +x "$OUT/eros/muse"
mkdir -p "$OUT/Music"   # drop albums/tracks here (folder-per-album)

# per-system rom folders (always) + a Bios/<system> only for cores that actually
# use a BIOS: pcsx_rearmed needs a real PS1 BIOS, mgba can optionally use a GBA
# one. Every other core runs without one, so we don't litter empty Bios/ folders.
# (awk, not sed: folder names contain spaces, so split on '|' and emit tab-sep.)
awk -F'|' '$1=="sys"{printf "%s\t%s\n", $3, $4}' "$ROOT/config/systems.cfg" |
while IFS="$(printf '\t')" read -r folder core; do
	mkdir -p "$OUT/Roms/$folder/.media"
	case "$core" in
		pcsx_rearmed|mgba) mkdir -p "$OUT/Bios/$folder" ;;
	esac
done

du -sh "$OUT"
echo "payload ready: $OUT"

# Zip for release distribution. The archive's CONTENTS are the SD-card root, so a
# user just extracts it onto a FAT32 card (matches the README Install step).
ZIP="$ROOT/out/EROS.zip"
rm -f "$ZIP"
( cd "$OUT" && zip -qr "$ZIP" . )
echo "release zip:  $ZIP  ($(du -h "$ZIP" | cut -f1))"
