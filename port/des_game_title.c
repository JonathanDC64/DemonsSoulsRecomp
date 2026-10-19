/* des_game_title.c: the exe's list of the port functions the lifted code calls
 * by name (DES_SPLIT, docs/57). The game DLL asks for each by name when it
 * loads (game_host.c's ps3_game_title_fn hook). The functions are declared
 * here as void(void) only to take their addresses, so this file includes no
 * header that declares them properly. */
#ifdef PS3_GAME_SPLIT
#include <string.h>
#include "des_game_title_funcs.h"   /* DES_GAME_TITLE_FUNCS, from the build directory */

#define DES_GT_DECL_(n) extern void n(void);
DES_GAME_TITLE_FUNCS(DES_GT_DECL_)

void* ps3_game_title_fn(const char* name)
{
#define DES_GT_FIND_(n) if (strcmp(name, #n) == 0) return (void*)&n;
    DES_GAME_TITLE_FUNCS(DES_GT_FIND_)
    return 0;
}
#endif
