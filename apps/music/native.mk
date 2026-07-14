# Native dev build of the EROS music player: make -f apps/music/native.mk
# Drivable with arrow keys / Return / Esc; point at a library with MUSIC_PATH
# and a font with EROS_MUSIC_FONT.
BUILD := build-native
SRC := apps/music/music.c apps/music/audio.c apps/music/library.c apps/music/player.c \
       apps/music/tags.c apps/music/third_party/stb_vorbis.c \
       src/platform.c src/coverflow.c

PKGS := sdl2 SDL2_image SDL2_ttf
CFLAGS := -O1 -g -Wall -Wextra -Wno-unused-parameter -std=gnu11 -D_GNU_SOURCE \
          $(shell pkg-config --cflags $(PKGS))
LDLIBS := $(shell pkg-config --libs $(PKGS)) -lm -lpthread

$(BUILD)/muse: $(SRC) $(wildcard apps/music/*.h) $(wildcard src/*.h)
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDLIBS)
