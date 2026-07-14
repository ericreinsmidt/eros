#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void strip(char *s)
{
	char *e = s + strlen(s);
	while (e > s && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t'))
		*--e = '\0';
	char *b = s;
	while (*b == ' ' || *b == '\t') b++;
	if (b != s) memmove(s, b, strlen(b) + 1);
}

/* split line on '|' into up to n fields (in place) */
static int split(char *line, char *fields[], int n)
{
	int c = 0;
	char *p = line;
	while (c < n) {
		fields[c++] = p;
		char *bar = strchr(p, '|');
		if (!bar) break;
		*bar = '\0';
		p = bar + 1;
	}
	for (int i = 0; i < c; i++) strip(fields[i]);
	return c;
}

bool cfg_load_systems(const char *path, systems_cfg *out)
{
	memset(out, 0, sizeof *out);
	FILE *f = fopen(path, "r");
	if (!f) return false;
	char line[1024];
	while (fgets(line, sizeof line, f) && out->count < CFG_MAX_CARDS) {
		strip(line);
		if (!line[0] || line[0] == '#') continue;
		char *fld[5] = { 0 };
		int n = split(line, fld, 5);
		if (n < 3) continue;
		card_cfg *c = &out->cards[out->count];
		if (strcmp(fld[0], "sys") == 0) c->kind = CARD_SYSTEM;
		else if (strcmp(fld[0], "app") == 0) c->kind = CARD_APP;
		else continue;
		snprintf(c->name, CFG_STR, "%s", fld[1]);
		snprintf(c->target, CFG_STR, "%s", fld[2]);
		if (n > 3) snprintf(c->core, CFG_STR, "%s", fld[3]);
		if (n > 4) snprintf(c->icon, CFG_STR, "%s", fld[4]);
		if (c->kind == CARD_SYSTEM && (!c->core[0] || !c->target[0])) continue;
		out->count++;
	}
	fclose(f);
	return out->count > 0;
}

void cfg_load_eros(const char *path, eros_cfg *out)
{
	memset(out, 0, sizeof *out);
	out->volume = -1;
	out->brightness = -1;
	FILE *f = fopen(path, "r");
	if (!f) return;
	char line[1024];
	while (fgets(line, sizeof line, f)) {
		strip(line);
		if (!line[0] || line[0] == '#') continue;
		char *eq = strchr(line, '=');
		if (!eq) continue;
		*eq = '\0';
		char *key = line, *val = eq + 1;
		strip(key); strip(val);
		if (strcmp(key, "bt_mac") == 0)
			snprintf(out->bt_mac, CFG_STR, "%s", val);
		else if (strcmp(key, "volume") == 0)
			out->volume = atoi(val);
		else if (strcmp(key, "brightness") == 0)
			out->brightness = atoi(val);
		else if (strcmp(key, "startup_system") == 0)
			snprintf(out->startup_system, CFG_STR, "%s", val);
	}
	fclose(f);
}
