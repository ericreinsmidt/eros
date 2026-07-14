# Runs inside the tg5040 toolchain container (CC/SYSROOT come from its env).
BUILD := build
SRC := src/main.c src/platform.c src/coverflow.c src/config.c
SRC := $(wildcard src/*.c)

CFLAGS := -O2 -mcpu=cortex-a53 -Wall -Wextra -Wno-unused-parameter -std=gnu11 \
          -I$(SYSROOT)/usr/include/SDL2 -D_GNU_SOURCE
LDLIBS := -lSDL2 -lSDL2_image -lSDL2_ttf -lm -ldl

$(BUILD)/eros.elf: $(SRC) $(wildcard src/*.h)
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDLIBS)

# Standalone libc-only helpers (no SDL). setbright + swread ship in the payload
# (setbright sets panel brightness before the boot animation; swread reports the
# BT audio switch so launch.sh can route in-game audio to a headset). evinject/
# evread are on-device input-debugging tools kept as a dev toolkit, not shipped.
$(BUILD)/setbright $(BUILD)/swread $(BUILD)/evinject $(BUILD)/evread: $(BUILD)/%: tools/%.c
	mkdir -p $(BUILD)
	$(CC) -O2 -mcpu=cortex-a53 -Wall -std=gnu11 -o $@ $<

.PHONY: tools
tools: $(BUILD)/setbright $(BUILD)/swread $(BUILD)/evinject $(BUILD)/evread
