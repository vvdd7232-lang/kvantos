/* ============================================================
 *  KvantOS 2.0 - persistent settings
 *
 *  A tiny key=value store kept as settings.cfg on KvFS. It is
 *  loaded once after the disk is mounted and saved whenever a
 *  setting changes. Without a mounted KvFS everything degrades
 *  to a no-op: the system still runs, settings just do not
 *  survive the reboot.
 *
 *  Current keys:
 *    lang   - interface language (en | ru)
 *    accent - accent colour index (-1 = keep the built-in default)
 *    wall   - desktop background index (-1 = keep the built-in default)
 * ============================================================ */
#include "kernel.h"

#define SETTINGS_FILE "settings.cfg"
#define SETTINGS_MAX  512

int settings_theme_accent = -1;
int settings_theme_wall   = -1;

static char buf[SETTINGS_MAX];

static int parse_int(const char *s) {
    int v = 0, neg = 0;
    if (*s == '-') { neg = 1; s++; }
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    return neg ? -v : v;
}

static void apply_line(const char *key, const char *val) {
    if (!strcmp(key, "lang")) {
        if (!strncmp(val, "ru", 2)) kv_lang_set(KV_LANG_RU);
        else if (!strncmp(val, "en", 2)) kv_lang_set(KV_LANG_EN);
    } else if (!strcmp(key, "accent")) {
        settings_theme_accent = parse_int(val);
    } else if (!strcmp(key, "wall")) {
        settings_theme_wall = parse_int(val);
    }
}

void settings_load(void) {
    if (!kvfs_mounted()) return;
    if (kvfs_read(SETTINGS_FILE, buf, SETTINGS_MAX - 1) <= 0) return;
    buf[SETTINGS_MAX - 1] = 0;

    /* parse line by line in place */
    char *p = buf;
    while (*p) {
        char *e = p;
        while (*e && *e != '\n') e++;
        char saved = *e;
        *e = 0;
        char *eq = p;
        while (*eq && *eq != '=') eq++;
        if (*eq == '=' && eq != p) {
            *eq = 0;
            apply_line(p, eq + 1);
            *eq = '=';
        }
        if (!saved) break;
        p = e + 1;
    }
}

void settings_save(void) {
    if (!kvfs_mounted()) return;
    ksnprintf(buf, SETTINGS_MAX, "lang=%s\naccent=%d\nwall=%d\n",
              kv_lang_get() == KV_LANG_RU ? "ru" : "en",
              settings_theme_accent, settings_theme_wall);
    kvfs_write(SETTINGS_FILE, buf, (u32)strlen(buf), 0);
}
