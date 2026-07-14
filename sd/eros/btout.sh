#!/bin/sh
# Game-audio-over-BT forwarder. Reads raw 48kHz/S16_LE/stereo PCM from the FIFO
# ($2) that minarch writes, and plays it to the bluealsa sink ($1) via aplay --
# a standalone process, because SDL/in-process ALSA can't drive bluealsa.
#
# BUF/PER trade latency against crackle: smaller = lower lag but underruns (a
# choppy/crackly stream) if the BT link jitters. Tune here -- no rebuild needed,
# it takes effect on the next game launch. The floor (~100-150ms) is SBC + the
# BT air + the headset's own buffer and can't be tuned away.
BUF=60000   # aplay ALSA buffer, microseconds
PER=15000   # aplay period, microseconds

exec aplay -q -t raw -f S16_LE -c 2 -r 48000 \
	--buffer-time="$BUF" --period-time="$PER" -D "$1" "$2"
