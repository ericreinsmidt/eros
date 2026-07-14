# Native dev build (macOS/Linux host): make -f mk/native.mk
# Drivable with arrow keys / Return / Esc; point it at a test tree with
# EROS_ROOT / EROS_ROMS / EROS_FONT etc.
BUILD := build-native
SRC := $(wildcard src/*.c)

PKGS := sdl2 SDL2_image SDL2_ttf
CFLAGS := -O1 -g -Wall -Wextra -Wno-unused-parameter -std=gnu11 -D_GNU_SOURCE \
          $(shell pkg-config --cflags $(PKGS))
LDLIBS := $(shell pkg-config --libs $(PKGS)) -lm

$(BUILD)/eros: $(SRC) $(wildcard src/*.h)
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDLIBS)
