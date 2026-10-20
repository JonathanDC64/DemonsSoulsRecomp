/* getenv_thunk.c: linked into instrumented (gen) builds only; tools/pgo/pgo.sh compiles it with /Zl and hands it to
 * lld-link through _LINK_.
 *
 * des_boot's main (ps3recomp's runtime/ppu/tests/boot_main.cpp) defines __imp_getenv itself: its getenv cache, so
 * the many getenv calls in the runtime and the lift do not each walk the environment. The profile runtime
 * (clang_rt.profile-x86_64.lib, built /MT) calls plain getenv, and the ucrt import member that defines getenv defines
 * __imp_getenv as well, so the link stops on a duplicate __imp_getenv. With getenv defined here, that member is never
 * pulled in, and the profile runtime's reads go through the same cache as everything else. */
extern char* (__cdecl* __imp_getenv)(const char*);

char* __cdecl getenv(const char* name)
{
    return __imp_getenv(name);
}
