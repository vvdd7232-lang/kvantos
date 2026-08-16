/* ============================================================
 *  KvantOS - language selection
 * ============================================================ */
#include "kernel.h"
#include "i18n.h"

/* English is the default: the project is published for an
   international audience. Russian is one command away. */
kv_lang_t kv_current_lang = KV_LANG_EN;

void kv_lang_set(kv_lang_t lang)
{
    kv_current_lang = (lang == KV_LANG_RU) ? KV_LANG_RU : KV_LANG_EN;
}

kv_lang_t kv_lang_get(void)
{
    return kv_current_lang;
}

const char *kv_lang_name(void)
{
    return (kv_current_lang == KV_LANG_RU) ? "Русский" : "English";
}

const char *kv_lang_code(void)
{
    return (kv_current_lang == KV_LANG_RU) ? "ru" : "en";
}
