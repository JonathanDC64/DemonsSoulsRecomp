/* des_region.cpp: the disc the game was lifted from (des_region.h), and the
 * European disc's language. */
#include "des_region.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

extern "C" uint8_t g_des_region = static_cast<uint8_t>(DesRegion::US);

const char* des_region_title_id(DesRegion r)
{
    return r == DesRegion::EU ? "BLES00932" : "BLUS30443";
}

extern "C" const char* cellGame_get_title_id(void);   /* cellGame.c: PARAM.SFO's TITLE_ID */
extern "C" void ps3_setenv(const char* name, const char* value);   /* boot_main.cpp: through the getenv cache */

/* From ps3_title_options_init, once PARAM.SFO is read: say which disc this is,
 * and warn when the game folder's PARAM.SFO is the other disc's (the program is
 * the lifted EBOOT's either way). */
extern "C" void des_region_check_title(void)
{
    const DesRegion r = des_region();
    const char* want = des_region_title_id(r);
    const char* id = cellGame_get_title_id();
    fprintf(stderr, "[des] disc: %s (%s)\n", want, r == DesRegion::EU ? "Europe" : "US");
    if (id && *id && strcmp(id, want) != 0)
        fprintf(stderr, "[des] the game folder's PARAM.SFO is %s, but the game was set up from %s's EBOOT\n", id, want);
}

namespace {

/* The European disc's languages: DES_LANGUAGE's values, and the PS3 system
 * language each one sets (PS3_SYS_LANG, ps3recomp's ps3emu/sys_lang.h). */
struct Language {
    const char* code;   /* DES_LANGUAGE, and the start of an OS language name */
    const char* sys;    /* PS3_SYS_LANG */
    const char* name;
};
constexpr Language kLanguages[] = {
    {"en", "en-gb", "English"},
    {"fr", "fr", "French"},
    {"de", "de", "German"},
    {"es", "es", "Spanish"},
    {"it", "it", "Italian"},
};

const Language* language_of(const std::string& code)
{
    for (const Language& l : kLanguages)
        if (code == l.code) return &l;
    return nullptr;
}

/* The OS's display language, as a lower-case language code ("fr"), or "" when
 * it gives none. Windows: the user's UI language. Elsewhere: LC_ALL, then
 * LC_MESSAGES, then LANG ("fr_FR.UTF-8"), as POSIX orders them. */
std::string os_language()
{
    std::string name;
#ifdef _WIN32
    wchar_t buf[LOCALE_NAME_MAX_LENGTH] = {};
    if (LCIDToLocaleName(MAKELCID(GetUserDefaultUILanguage(), SORT_DEFAULT), buf, LOCALE_NAME_MAX_LENGTH, 0) > 0)
        for (const wchar_t* p = buf; *p && *p < 128; p++) name += static_cast<char>(*p);
#else
    for (const char* var : {"LC_ALL", "LC_MESSAGES", "LANG"}) {
        const char* v = getenv(var);
        if (v && *v) {
            name = v;
            break;
        }
    }
#endif
    std::string code;
    for (char c : name) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        if (c < 'a' || c > 'z') break;
        code += c;
    }
    return code;
}

}  // namespace

/* From ps3_title_options_init, after settings.ini is read and before the guest
 * runs: the European disc's text language. DES_LANGUAGE picks one of the
 * disc's five; empty (the default) is the PC's language when it is one of
 * them, else English. A PS3_SYS_LANG given at start wins. The US disc is
 * English only, so it keeps the runtime's English (US). */
extern "C" void des_region_language(void)
{
    if (des_region() != DesRegion::EU) return;
    if (const char* given = getenv("PS3_SYS_LANG"); given && *given) {
        fprintf(stderr, "[des] language: PS3_SYS_LANG=%s\n", given);
        return;
    }
    const char* chosen = getenv("DES_LANGUAGE");
    const bool from_pc = !chosen || !*chosen;
    const std::string code = from_pc ? os_language() : std::string(chosen);
    const Language* l = language_of(code);
    if (!l) {
        if (!from_pc) fprintf(stderr, "[des] DES_LANGUAGE=%s is not one of the disc's languages\n", chosen);
        l = &kLanguages[0];
    }
    ps3_setenv("PS3_SYS_LANG", l->sys);
    fprintf(stderr, "[des] language: %s%s\n", l->name, from_pc ? " (the PC's language, else English)" : "");
}
