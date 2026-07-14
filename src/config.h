#ifndef EROS_CONFIG_H
#define EROS_CONFIG_H

#include <stdbool.h>

#define CFG_MAX_CARDS 32
#define CFG_STR 256

typedef enum { CARD_SYSTEM, CARD_APP } card_kind;

typedef struct {
	card_kind kind;
	char name[CFG_STR];    /* display name */
	char target[CFG_STR];  /* rom folder name (sys) or exec path (app) */
	char core[CFG_STR];    /* core short name, e.g. "snes9x" (sys only) */
	char icon[CFG_STR];    /* icon filename under eros/icons */
} card_cfg;

typedef struct {
	card_cfg cards[CFG_MAX_CARDS];
	int count;
} systems_cfg;

typedef struct {
	char bt_mac[CFG_STR];
	int volume;            /* 0..100, -1 = unset */
	int brightness;        /* 0..10, -1 = unset */
	char startup_system[CFG_STR];
} eros_cfg;

bool cfg_load_systems(const char *path, systems_cfg *out);
void cfg_load_eros(const char *path, eros_cfg *out);

#endif
