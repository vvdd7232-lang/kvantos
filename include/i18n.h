/* ============================================================
 *  KvantOS - runtime language switching
 *
 *  The system speaks English by default and can switch to
 *  Russian at runtime. Both variants are compiled in: there is
 *  no filesystem at early boot, so external locale files are
 *  not an option.
 *
 *  Usage:
 *      kputs(T("Files", "Файлы"));
 *
 *  T() expands to a function call, so it must NOT be used in
 *  static initialisers. For tables of translatable strings keep
 *  both variants side by side and pick one with kv_pick():
 *
 *      static const char *names_en[] = { "Files", "Terminal" };
 *      static const char *names_ru[] = { "Файлы", "Терминал"  };
 *      const char *n = kv_pick(names_en[i], names_ru[i]);
 * ============================================================ */
#ifndef KV_I18N_H
#define KV_I18N_H

typedef enum { KV_LANG_EN = 0, KV_LANG_RU = 1 } kv_lang_t;

/* Current language. Read it through kv_lang()/kv_pick(), the
   variable itself is only exported for the fast path below. */
extern kv_lang_t kv_current_lang;

void        kv_lang_set(kv_lang_t lang);
kv_lang_t   kv_lang_get(void);
const char *kv_lang_name(void);          /* "English" / "Русский" */
const char *kv_lang_code(void);          /* "en" / "ru"           */

/* Chooses one of the two variants. Marked inline: this is called
   thousands of times per frame while drawing the desktop. */
static inline const char *kv_pick(const char *en, const char *ru)
{
    return (kv_current_lang == KV_LANG_RU) ? ru : en;
}

#define T(en, ru) kv_pick((en), (ru))

#endif /* KV_I18N_H */
