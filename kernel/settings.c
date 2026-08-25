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
 * ============================================================ */
#include "kernel.h"

#define SETTINGS_FILE "settings.cfg"
#define SETTINGS_MAX  512

static char buf[SETTINGS_MAX];

static void apply_line(const char *key, const char *val) {
    if (!strcmp(key, "lang")) {
        if (!strncmp(val, "ru", 2)) kv_lang_set(KV_LANG_RU);
        else if (!strncmp(val, "en", 2)) kv_lang_set(KV_LANG_EN);
    }
    /* further keys go here */
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
    ksnprintf(buf, SETTINGS_MAX, "lang=%s\n",
              kv_lang_get() == KV_LANG_RU ? "ru" : "en");
    kvfs_write(SETTINGS_FILE, buf, (u32)strlen(buf), 0);
}
