# Runs inside the tg5040 toolchain container (CC/SYSROOT from its env).
# Builds the EROS music player binary: build/muse
BUILD := build
SRC := apps/music/music.c apps/music/audio.c apps/music/library.c apps/music/player.c \
       apps/music/tags.c apps/music/third_party/stb_vorbis.c \
       src/platform.c src/coverflow.c

CFLAGS := -O2 -mcpu=cortex-a53 -Wall -Wextra -Wno-unused-parameter -std=gnu11 \
          -I$(SYSROOT)/usr/include/SDL2 -D_GNU_SOURCE
LDLIBS := -lSDL2 -lSDL2_image -lSDL2_ttf -lasound -lm -ldl -lpthread

$(BUILD)/muse: $(SRC) $(wildcard apps/music/*.h) $(wildcard src/*.h)
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDLIBS)
