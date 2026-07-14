#!/bin/sh
# EROS boot entry. Called from .tmp_update/tg5040.sh; never returns.

EROS_DIR=/mnt/SDCARD/eros
SDCARD=/mnt/SDCARD

export PLATFORM=tg5040
export DEVICE=brick
export SDCARD_PATH=$SDCARD
export BIOS_PATH=$SDCARD/Bios
export ROMS_PATH=$SDCARD/Roms
export SAVES_PATH=$SDCARD/Saves
export CHEATS_PATH=$SDCARD/Cheats
export SYSTEM_PATH=$EROS_DIR
export CORES_PATH=$EROS_DIR/cores
export USERDATA_PATH=$SDCARD/.userdata/tg5040
export SHARED_USERDATA_PATH=$SDCARD/.userdata/shared
export LOGS_PATH=$USERDATA_PATH/logs
export HOME=$USERDATA_PATH
export LD_LIBRARY_PATH=$EROS_DIR/lib:/usr/trimui/lib:$LD_LIBRARY_PATH
export PATH=/usr/trimui/bin:$PATH

mkdir -p "$BIOS_PATH" "$ROMS_PATH" "$SAVES_PATH" "$USERDATA_PATH" "$LOGS_PATH" \
         "$SHARED_USERDATA_PATH"

# Clear any stale .asoundrc from an earlier build: EROS routes in-game BT audio
# via AUDIODEV (the BT loop publishes the device to /tmp/eros_bt_route), not
# minarch's .asoundrc watcher -- a leftover one would wrongly redirect ALSA's
# default PCM and could stall a game's audio open.
rm -f "$USERDATA_PATH/.asoundrc"

CFG=$EROS_DIR/eros.cfg
getcfg() { [ -f "$CFG" ] && sed -n "s/^$1=//p" "$CFG" | tail -1; }

# All LEDs off. A function so it can run before the animation and again after
# trimui_inputd (which re-lights them on start).
leds_off() {
	echo 0 > /sys/class/led_anim/effect_enable 2> /dev/null
	for g in l r lr m f1 f2; do echo "000000 " > /sys/class/led_anim/effect_rgb_hex_$g 2> /dev/null; done
	echo 0 > /sys/class/led_anim/max_scale 2> /dev/null
	echo 0 > /sys/class/led_anim/max_scale_lr 2> /dev/null
	echo 0 > /sys/class/led_anim/max_scale_f1f2 2> /dev/null
	for f in /sys/class/leds/sunxi_led*/brightness; do echo 0 > "$f" 2> /dev/null; done
}

# Brightness: set the configured level now (raw table matches libmsettings) so
# the boot animation isn't dim, since eros.elf hasn't applied it yet.
brightness_raw() {
	case "$(getcfg brightness)" in
		0) echo 1;; 1) echo 8;; 2) echo 16;; 3) echo 32;; 4) echo 48;; 5) echo 72;;
		6) echo 96;; 7) echo 128;; 8) echo 160;; 9) echo 192;; 10) echo 255;; *) echo 160;;
	esac
}
[ -x "$EROS_DIR/setbright" ] && "$EROS_DIR/setbright" "$(brightness_raw)"

# LEDs off before the animation.
leds_off

# One-time: replace the stock TrimUI boot splash on the boot partition with our
# black bootlogo (u-boot reads bootlogo.bmp from /dev/mmcblk0p1). Guarded by a
# marker so it only touches the eMMC once; the stock logo is backed up.
if [ -f "$EROS_DIR/bootlogo.bmp" ] && [ ! -f "$EROS_DIR/.bootlogo_applied" ]; then
	mkdir -p /mnt/boot
	if mount -t vfat /dev/mmcblk0p1 /mnt/boot 2> /dev/null; then
		if [ -f /mnt/boot/bootlogo.bmp ] && [ ! -f "$EROS_DIR/bootlogo.stock.bmp" ]; then
			cp /mnt/boot/bootlogo.bmp "$EROS_DIR/bootlogo.stock.bmp"
		fi
		cp "$EROS_DIR/bootlogo.bmp" /mnt/boot/bootlogo.bmp && sync
		umount /mnt/boot && touch "$EROS_DIR/.bootlogo_applied"
	fi
fi

# One-time: replace the stock TrimUI "loading" splash (/etc/splash.png, blitted
# by pic2fb in the runtrimui init service just before our boot) with the EROS
# splash. The rootfs is a writable overlay, so this persists.
if [ -f "$EROS_DIR/splash.png" ] && [ ! -f "$EROS_DIR/.splash_applied" ]; then
	if [ -f /etc/splash.png ] && [ ! -f "$EROS_DIR/splash.stock.png" ]; then
		cp /etc/splash.png "$EROS_DIR/splash.stock.png"
	fi
	cp "$EROS_DIR/splash.png" /etc/splash.png && sync && touch "$EROS_DIR/.splash_applied"
fi

# One-time: match the boot loading-splash brightness to the launcher. pic2fb (in
# /etc/init.d/runtrimui) blits the splash at the hardware-default brightness,
# which differs from the level EROS uses -- so the screen visibly jumps partway
# through boot. Install a raw-brightness helper and call it before pic2fb, so
# splash -> animation -> launcher are one consistent brightness. Reversible: the
# stock init is backed up to runtrimui.eros-bak.
if [ -x "$EROS_DIR/setbright" ] && [ ! -f "$EROS_DIR/.brightboot_applied" ]; then
	cp "$EROS_DIR/setbright" /usr/trimui/bin/setbright 2> /dev/null && chmod +x /usr/trimui/bin/setbright
	cat > /usr/trimui/bin/eros-bootbright.sh <<'BB'
#!/bin/sh
# Set panel brightness to match the EROS launcher, before the boot splash.
BR=$(sed -n 's/^brightness=//p' /mnt/SDCARD/eros/eros.cfg 2> /dev/null | tail -1)
case "$BR" in
	0) R=1;; 1) R=8;; 2) R=16;; 3) R=32;; 4) R=48;; 5) R=72;;
	6) R=96;; 7) R=128;; 8) R=160;; 9) R=192;; 10) R=255;; *) R=160;;
esac
[ -x /usr/trimui/bin/setbright ] && /usr/trimui/bin/setbright "$R"
BB
	chmod +x /usr/trimui/bin/eros-bootbright.sh
	if [ -f /etc/init.d/runtrimui ] && ! grep -q eros-bootbright /etc/init.d/runtrimui; then
		cp /etc/init.d/runtrimui /etc/init.d/runtrimui.eros-bak
		awk '/pic2fb/ && !d {print "/usr/trimui/bin/eros-bootbright.sh"; d=1} {print}' \
			/etc/init.d/runtrimui.eros-bak > /etc/init.d/runtrimui
		sh -n /etc/init.d/runtrimui 2> /dev/null || cp /etc/init.d/runtrimui.eros-bak /etc/init.d/runtrimui
		chmod +x /etc/init.d/runtrimui
	fi
	sync
	touch "$EROS_DIR/.brightboot_applied"
fi

# Boot animation: play the full clip once per boot to the framebuffer, before
# the frontend. The last frame (the EROS logo) holds until Cover Flow draws.
if [ -f "$EROS_DIR/EROS-boot.mp4" ]; then
	# stop at ~4.3s: the logo is fully formed by then and the clip's tail is a
	# static hold, so cutting it just trims dead time (the last drawn frame stays
	# on the fb until Cover Flow). File is left full; -t only limits playback.
	ffmpeg -hide_banner -loglevel quiet -re -i "$EROS_DIR/EROS-boot.mp4" -t 4.3 \
	       -pix_fmt bgra -f fbdev /dev/fb0 2> /dev/null
fi

# Rumble off, mute-switch gpio readable
echo 227 > /sys/class/gpio/export 2> /dev/null
echo -n out > /sys/class/gpio/gpio227/direction 2> /dev/null
echo -n 0 > /sys/class/gpio/gpio227/value 2> /dev/null

# WiFi is off unless eros.cfg says wifi=1 (a .devwifi marker forces it on too,
# for development units that must stay reachable over ssh).
WIFI=$(getcfg wifi)
if [ "$WIFI" = "1" ] || [ -f "$EROS_DIR/.devwifi" ]; then
	if [ -f "$EROS_DIR/wpa.conf" ]; then
		# fresh rootfs (no saved network): connect from creds on the card
		(
			rfkill unblock wifi 2> /dev/null
			ifconfig wlan0 up 2> /dev/null
			wpa_supplicant -B -i wlan0 -c "$EROS_DIR/wpa.conf" 2> /dev/null
			udhcpc -i wlan0 -b 2> /dev/null
		) &
	else
		sh /etc/wifi/wifi_init.sh start > /dev/null 2>&1 &
	fi
else
	# WiFi off: stop the supplicant/DHCP and drop the interface, but do NOT
	# rfkill the radio -- the AIC8800 is a combo BT/WiFi chip and blocking it
	# takes Bluetooth down too. Leaving the driver loaded keeps BT working.
	/etc/init.d/wpa_supplicant stop 2> /dev/null
	killall udhcpc wpa_supplicant 2> /dev/null
	ifconfig wlan0 down 2> /dev/null
fi

# CPU: interactive scaling
echo interactive > /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2> /dev/null

# Buttons arrive via the stock GPIO input daemon's virtual joystick.
pgrep trimui_inputd > /dev/null || trimui_inputd &

# All LEDs off again (trimui_inputd re-enables them on start).
sleep 1
leds_off

# Optional: BT sink auto-connect (pair once via ssh). Volume and brightness
# are applied by eros.elf itself via libmsettings.
# Bluetooth on (the design keeps WiFi off and BT available). Bring the radio
# up so it's ready to pair/use; auto-connect a sink if eros.cfg names one.
(
	rfkill unblock bluetooth > /dev/null 2>&1
	/etc/bluetooth/bt_init.sh start > /dev/null 2>&1
	bluetoothctl power on > /dev/null 2>&1

	# Start the audio backend unconditionally, so pairing a NEW headset from the
	# launcher works even when nothing is remembered yet. a2dp-source carries the
	# music; hfp-ag adds the Hands-Free gateway role so the headset connects like
	# it would to a phone and speaks its "connected" prompt (A2DP-only just beeps).
	# SCO/HFP stays idle (no calls); music still routes over A2DP. --a2dp-volume
	# puts volume in native (pass-through) mode, NOT local SoftVolume: the device
	# does not attenuate the stream, so the headset controls its own volume with no
	# device-side cap. (Headsets here don't report their button presses back to us,
	# so the device just stays out of BT volume entirely -- the headset owns it.)
	bluealsa -p a2dp-source -p hfp-ag --a2dp-volume > /dev/null 2>&1 &
	sleep 3   # let both profiles register with BlueZ before the first connect

	# Keep the remembered sink connected whenever it's reachable, so powering the
	# headset back on reconnects it anywhere (Cover Flow, a game, Muse) -- not just
	# when the music app happens to be open. bt_mac is re-read every pass, so a sink
	# the launcher just paired is picked up here without a reboot. While the launcher
	# drives its own scan/pair it creates /tmp/eros_bt_pause; we stand down then so
	# two bluetoothctl users can't collide on D-Bus. The live state is published to
	# /tmp/eros_bt_state for the launcher's BT screen. Judge "connected" by the real
	# state: bluetoothctl connect returns Failed for a2dp even when the link came up.
	hfp_ok=0
	trusted=""

	# In-game (minarch) audio routing. We publish the ALSA device string a game
	# should open to /tmp/eros_bt_route; the launcher reads it at launch and sets
	# AUDIODEV, so minarch's SDL opens the bluealsa PCM directly (the same string
	# aplay/Muse use). Routing to the headset only when the switch is on BT keeps
	# games consistent with Muse. We deliberately do NOT drive minarch's native
	# .asoundrc watcher: opening the plug->bluealsa "default" that way stalled and
	# then hung the reset; the explicit device string plays cleanly. route_mac
	# tracks what we last published ("" = speaker/none).
	ROUTE_F="/tmp/eros_bt_route"
	SWREAD="$EROS_DIR/swread"
	route_mac=""
	set_route() {   # $1 = sink MAC
		[ "$route_mac" = "$1" ] && return
		printf 'bluealsa:DEV=%s,PROFILE=a2dp' "$1" > "$ROUTE_F"
		route_mac="$1"
	}
	clear_route() {
		[ -z "$route_mac" ] && return
		rm -f "$ROUTE_F"
		route_mac=""
	}

	while :; do
		[ -f /tmp/eros_bt_pause ] && { sleep 2; continue; }
		mac=$(getcfg bt_mac)
		if [ -z "$mac" ]; then
			echo 0 > /tmp/eros_bt_state
			clear_route
			hfp_ok=0; trusted=""
			sleep 5
			continue
		fi
		[ "$mac" != "$trusted" ] && { bluetoothctl trust "$mac" > /dev/null 2>&1; trusted="$mac"; }
		if bluetoothctl info "$mac" 2> /dev/null | grep -q "Connected: yes"; then
			echo 1 > /tmp/eros_bt_state
			# If the headset raced in A2DP-only before hfp-ag was ready, top up the
			# HFP profile once -- a plain connect adds the missing profile (no
			# disconnect), which is what makes it announce "connected". The sco PCM
			# appears in bluealsa-aplay -L once HFP is linked.
			if [ "$hfp_ok" = 0 ]; then
				bluealsa-aplay -L 2> /dev/null | grep -q "DEV=$mac,PROFILE=sco" \
					|| bluetoothctl connect "$mac" > /dev/null 2>&1
				hfp_ok=1
			fi
			# Route a game to the headset only when the switch is on BT (speaker off);
			# that keeps games consistent with Muse, which follows the same switch.
			if "$SWREAD"; then set_route "$mac"; else clear_route; fi
			# Poll faster while a game owns audio, so a headset drop or switch flip
			# reaches minarch's device-watch within a few seconds; idle/music is gentle.
			if pgrep -f minarch.elf > /dev/null 2>&1; then sleep 5; else sleep 30; fi
		else
			echo 0 > /tmp/eros_bt_state
			hfp_ok=0
			clear_route
			bluetoothctl connect "$mac" > /dev/null 2>&1
			sleep 5
		fi
	done
) &

rm -f /tmp/eros_poweroff

cd "$EROS_DIR"
FAILS=0
while : ; do
	START=$(cat /proc/uptime | cut -d. -f1)
	./eros.elf > "$LOGS_PATH/eros.log" 2>&1
	[ -f /tmp/eros_poweroff ] && break
	END=$(cat /proc/uptime | cut -d. -f1)
	if [ $((END - START)) -lt 5 ]; then
		FAILS=$((FAILS + 1))
		[ $FAILS -ge 5 ] && break
	else
		FAILS=0
	fi
	sleep 1
done

sync
poweroff
sleep 10
