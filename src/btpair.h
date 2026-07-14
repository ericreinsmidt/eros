#ifndef EROS_BTPAIR_H
#define EROS_BTPAIR_H

#include <SDL.h>

#include "platform.h"

/* Launcher-owned Bluetooth pair/connect screen.
 *
 * When the physical output switch is on BT but no sink is connected, this takes
 * over the display: a short "Connecting…" grace (giving the boot/reconnect loop
 * time), then a scan → pick → pair flow. Pairing uses a classic BR/EDR discovery
 * and a full bond so a real link key is written -- an LE-only scan never finds a
 * classic A2DP headset, and a keyless bond never reconnects. bluetoothctl runs
 * out-of-process (fork+exec) so the UI stays responsive; results come back in
 * small files under /tmp. */

/* One-time setup: open the BT switch, load fonts, read bt_mac from eros.cfg. */
void btpair_init(const char *cfg_path);

/* Pump the physical switch (cheap; call every frame). */
void btpair_poll(void);

/* Re-evaluate engagement from switch position + connection state + grace.
 * Call every frame before btpair_active(). */
void btpair_tick(void);

/* True when the BT screen owns the display this frame (route input/render here
 * instead of Cover Flow). */
bool btpair_active(void);

/* Drive the engaged screen (scan/list/pair; B dismisses to Cover Flow). */
void btpair_update(in_state *in);

/* Draw the engaged screen and present. */
void btpair_render(SDL_Renderer *r);

/* Remembered sink MAC, updated after a successful pair ("" if none). */
const char *btpair_mac(void);

#endif
