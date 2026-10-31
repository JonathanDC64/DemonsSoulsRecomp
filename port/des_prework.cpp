/*
 * des_prework.cpp: Demon's Souls' startup work, with the window open and
 * before the game runs (boot_main's ps3_title_prework hook).
 *
 * Fills the shader cache from the game's shader archives, so no draw waits on
 * the shader compiler: without it a new area stalled on dozens of compiles
 * (~40 ms each, 100+ ms for the large vertex programs). This was
 * tools/des/precompile_shaders.sh, run by play.sh before the game started:
 * ~20 s the first time and ~4 s over a full cache, with no window.
 *
 * The archives are USRDIR/shader/*.shaderbnd: BND3, big-endian. Each entry is
 * a FromSoftware header (its size in the second word) followed by the
 * CgBinaryProgram the game uploads. The game's executable (EBOOT.elf) holds 8
 * more, the engine's own (docs/30): the pass finds them by their headers.
 * The work is ps3recomp's rsx_shader_precompile; the wrapper UI shows its
 * progress (des_prework_status) and can skip it (des_prework_skip).
 *
 * The programs of the archives the game draws without a depth test (kFlat)
 * and the executable's are built a second time in the form a scaled target
 * (RSX_SCALE above 100) draws them, with the sprite-edge clamp
 * (RSX_PRECOMPILE_2D). Without it a first start at 200% compiled 16 of them
 * in play, most in one 280-500 ms frame as the level appeared (docs/93).
 *
 * A stamp in the cache (precompile.stamp) records the archives, the
 * executables (this program and the game's) and the cache's fp_variants.txt
 * the last full pass saw; when none of them changed, the pass is skipped.
 *
 * Setup runs the same pass once the game is built (des_prework_shaders,
 * `des_host --shaders <game folder> <EBOOT.elf>`), so the first start finds
 * the cache full.
 *
 * The pass also takes the pipeline index of the game's version
 * (port/data/pso_index/<TITLE_ID>-<APP_VER>.tsv, docs/97): the pipelines
 * players' games have asked for, each as fingerprints of its two programs
 * and its draw state. ps3recomp finds each row's programs among the queued
 * ones, puts their shaders in the cache, and builds the pipelines in the
 * background once the game runs, so a draw loads its pipeline instead of
 * waiting on the graphics driver. When the cache is up to date but the
 * index's pipelines are not all built yet (pso_index.built), the pass does
 * only that. The game records the pipelines it asks for in the cache
 * (pso_index_seen.tsv), and tools/des/pso_index.py merges those into the
 * index. DES_PSO_INDEX=<file> takes another index, =0 none.
 *
 * DES_PRECOMPILE=0 skips it at a start; =force runs it whatever the stamp says.
 *
 * With ps3recomp's portable renderer (PS3RECOMP_RSX_HAL) the pass runs once
 * for each target the runtime has (rsx_shader_precompile.h): d3d12, today's
 * renderer's, as above; dxbc, the new D3D12 path's, into the same .dxbc
 * files; spv, Vulkan's, into <cache>/gpu/spv/. Each later target has its own
 * stamp, <cache>/gpu/precompile_<target>.stamp, which also records its
 * compiler; a target whose stamp is up to date is skipped. It runs off
 * Windows too, where only spv is.
 */
#include "ps3emu/ps3_knob.h"
#include "ui/include/des_ui.h"  /* des_ui_privacy_wait */
#include "rsx_shader_precompile.h"
#include "des_rsx_route.h"  /* the renderer calls below: through rsx_out in a PS3RECOMP_RSX_HAL build */

#ifdef _WIN32
#include <windows.h>
#endif

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

extern "C" const char* ppu_vfs_root;
extern "C" const char* rsx_d3d12_shader_cache_path(void);
extern "C" void rsx_d3d12_pso_index_game(const char* game);
extern "C" void cellGame_init_from_paramsfo(const char* sfo_path);
extern "C" const char* cellGame_get_title_id(void);
extern "C" const char* cellGame_get_app_ver(void);

namespace {

/* Unit kinds Demon's Souls binds (seen in play, PS3_SHADER_CACHE_HLSL=1):
 * the shadow map on 7, cube maps on 8 and 11 (12-14 also hold cubes when
 * bound); post effects read the depth buffer as colour on 1 or 2. Variants
 * the game has used are rebuilt from the cache's fp_variants.txt too, so
 * this only has to cover a first run. */
constexpr const char* kUnits = "7=shadow,8=cube,11=cube,12=cube,13=cube,14=cube";
constexpr const char* kAlts  = "1=d2c,2=d2c";

/* The archives whose programs the game draws without a depth test: the post
 * filters, the menus and HUD, the debug font. The 14 archive programs a first
 * start at 200% compiled with the clamp in play came from these (11, 2 and 1;
 * docs/93). ds_flver's and ds_sfx's draws have a depth test. */
constexpr const char* kFlat[] = {"ds_filter.shaderbnd", "ds_menu.shaderbnd", "dbgfont.shaderbnd"};

/* The game's executable (main's ELF, des_prework_elf); empty if unknown. */
std::string s_elf;

std::atomic<int> s_state{0};   /* 0 idle, 1 reading, 2 compiling, 3 done */
std::atomic<bool> s_skip{false};

#ifdef _WIN32
constexpr const char* kSep = "\\";
#else
constexpr const char* kSep = "/";
#endif

uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

/* Case not counted. */
bool same_nocase(const char* a, const char* b)
{
    for (; *a && *b; a++, b++)
        if (std::tolower((unsigned char)*a) != std::tolower((unsigned char)*b)) return false;
    return *a == *b;
}

bool read_file(const std::string& path, std::vector<uint8_t>& out)
{
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    out.resize(n > 0 ? size_t(n) : 0);
    const bool ok = n > 0 && std::fread(out.data(), 1, out.size(), f) == out.size();
    std::fclose(f);
    return ok;
}

/* The game and its version as PARAM.SFO has them, TITLE_ID-APP_VER ("BLUS30443-01.00"): what the
 * pipeline index is kept by. */
std::string game_tag()
{
    const char* id = cellGame_get_title_id();
    const char* ver = cellGame_get_app_ver();
    return std::string(id ? id : "") + "-" + (ver ? ver : "");
}

/* This program's folder; empty if it cannot be told. */
#ifdef _WIN32
std::filesystem::path exe_dir()
{
    wchar_t buf[4096];
    const DWORD n = GetModuleFileNameW(nullptr, buf, DWORD(std::size(buf)));
    return n && n < std::size(buf) ? std::filesystem::path(buf).parent_path() : std::filesystem::path();
}
#else
std::filesystem::path exe_dir()
{
    std::error_code ec;
    const std::filesystem::path p = std::filesystem::read_symlink("/proc/self/exe", ec);
    return ec ? std::filesystem::path() : p.parent_path();
}
#endif

bool read_text(const std::filesystem::path& path, std::string& out)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

/* The pipeline index for the game's version: DES_PSO_INDEX's file when it is set (0: none), else
 * port/data/pso_index/<game_tag>.tsv in this program's folder or up to three above it (a release has
 * port/ beside the exe, a build tree further up). Empty if there is none; `name` says which it was. */
std::string read_index(std::string& name)
{
    std::string text;
    const char* knob = std::getenv("DES_PSO_INDEX");
    if (knob && std::strcmp(knob, "0") == 0) return text;
    if (knob && *knob) {
        name = knob;
        if (!read_text(std::filesystem::path(knob), text))
            std::fprintf(stderr, "[des] shaders: no pipeline index at %s\n", knob);
        return text;
    }
    name = game_tag() + ".tsv";
    std::error_code ec;
    std::filesystem::path dir = exe_dir();
    for (int up = 0; up <= 3 && !dir.empty(); ++up) {
        const std::filesystem::path f = dir / "port" / "data" / "pso_index" / name;
        if (std::filesystem::is_regular_file(f, ec)) {
            read_text(f, text);
            return text;
        }
        if (dir == dir.parent_path()) break;
        dir = dir.parent_path();
    }
    std::fprintf(stderr, "[des] shaders: no pipeline index for %s\n", game_tag().c_str());
    return text;
}

#ifdef _WIN32
/* One line per input: name, size, last write. */
void stamp_line(std::string& s, const char* name, const WIN32_FILE_ATTRIBUTE_DATA& a)
{
    char line[512];
    std::snprintf(line, sizeof line, "%s %lu:%lu %lu:%lu\n", name, a.nFileSizeHigh, a.nFileSizeLow,
                  a.ftLastWriteTime.dwHighDateTime, a.ftLastWriteTime.dwLowDateTime);
    s += line;
}

void stamp_file(std::string& s, const char* label, const std::string& path)
{
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &a)) stamp_line(s, label, a);
    else s += std::string(label) + " none\n";
}

/* The archives, in name order, and the stamp of everything the pass depends on. */
std::vector<std::string> list_archives(const std::string& dir, std::string& stamp)
{
    std::vector<std::string> names;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "\\*.shaderbnd").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do names.emplace_back(fd.cFileName);
        while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    std::sort(names.begin(), names.end());
    stamp = "des_prework 2\n";   /* 2: the 2D forms and the executable's programs */
    /* The build, from its PE header (link time and image size): a copy of
     * the same executable elsewhere, or with a new file time, is the same. */
    {
        const auto* base = reinterpret_cast<const uint8_t*>(GetModuleHandleA(nullptr));
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        char line[64];
        std::snprintf(line, sizeof line, "exe %08lX %08lX\n", (unsigned long)nt->FileHeader.TimeDateStamp,
                      (unsigned long)nt->OptionalHeader.SizeOfImage);
        stamp += line;
    }
    stamp += std::string("kinds ") + kUnits + " " + kAlts + "\n";
    for (const std::string& n : names) stamp_file(stamp, n.c_str(), dir + "\\" + n);
    return names;
}
#else
/* One line per input: name, size, last write. */
void stamp_file(std::string& s, const char* label, const std::string& path)
{
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) {
        s += std::string(label) + " none\n";
        return;
    }
    const auto when = std::filesystem::last_write_time(path, ec).time_since_epoch().count();
    char line[512];
    std::snprintf(line, sizeof line, "%s %llu %lld\n", label, (unsigned long long)size, (long long)when);
    s += line;
}

/* The archives, in name order, and the stamp of everything the pass depends on. */
std::vector<std::string> list_archives(const std::string& dir, std::string& stamp)
{
    std::vector<std::string> names;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec))
        if (e.path().extension() == ".shaderbnd") names.emplace_back(e.path().filename().string());
    std::sort(names.begin(), names.end());
    stamp = "des_prework 2\n";   /* 2: the 2D forms and the executable's programs */
    stamp_file(stamp, "exe", "/proc/self/exe");   /* (a new build is another size or time) */
    stamp += std::string("kinds ") + kUnits + " " + kAlts + "\n";
    for (const std::string& n : names) stamp_file(stamp, n.c_str(), dir + "/" + n);
    return names;
}
#endif

bool flat_archive(const std::string& name)
{
    for (const char* f : kFlat)
        if (same_nocase(name.c_str(), f)) return true;
    return false;
}

/* Queue every .vpo/.fpo in one archive; returns how many. */
int queue_archive(const std::string& path, const std::string& name)
{
    const unsigned flags = flat_archive(name) ? RSX_PRECOMPILE_2D : 0u;
    std::vector<uint8_t> b;
    if (!read_file(path, b) || b.size() < 0x20 || std::memcmp(b.data(), "BND3", 4) != 0) {
        std::fprintf(stderr, "[des] shaders: %s is not a BND3 archive\n", name.c_str());
        return 0;
    }
    const uint32_t count = be32(&b[0x10]);
    int n = 0;
    for (uint32_t i = 0; i < count; i++) {
        const size_t o = 0x20 + size_t(i) * 0x18;
        if (o + 0x18 > b.size()) break;
        const uint32_t size = be32(&b[o + 4]), off = be32(&b[o + 8]), name_off = be32(&b[o + 16]);
        if (off > b.size() || size > b.size() - off || name_off >= b.size() || size < 8) continue;
        const char* ename = reinterpret_cast<const char*>(&b[name_off]);
        const size_t elen = strnlen(ename, b.size() - name_off);
        if (elen < 4) continue;
        const std::string ext(ename + elen - 4, 4);
        if (!same_nocase(ext.c_str(), ".vpo") && !same_nocase(ext.c_str(), ".fpo")) continue;
        const uint32_t hdr = be32(&b[off + 4]);
        if (hdr >= size) continue;
        char label[256];
        std::snprintf(label, sizeof label, "%s#%u", name.c_str(), i);
        rsx_shader_precompile_add_flags(label, &b[off + hdr], size - hdr, flags);
        n++;
    }
    return n;
}

/* Queue the Cg programs in the game's executable (the engine's post and
 * display passes, docs/30); returns how many. They sit in its data as
 * the SDK's CgBinaryProgram: profile sce_vp_rsx 0x1B5B or sce_fp_rsx 0x1B5C
 * at a 4-byte boundary, format revision 6, then sizes and offsets that stay
 * inside the program, and microcode in 16-byte instructions. On the US disc
 * that is 8 programs and nothing else. */
int queue_elf(const std::string& path)
{
    std::vector<uint8_t> b;
    if (path.empty() || !read_file(path, b)) return 0;
    int n = 0;
    for (size_t o = 0; o + 32 <= b.size(); o += 4) {
        const uint32_t profile = be32(&b[o]);
        if ((profile != 0x1B5Bu && profile != 0x1B5Cu) || be32(&b[o + 4]) != 6) continue;
        const uint32_t total = be32(&b[o + 8]), params = be32(&b[o + 16]), prog = be32(&b[o + 20]);
        const uint32_t usz = be32(&b[o + 24]), uoff = be32(&b[o + 28]);
        if (total < 32 || total > b.size() - o || params > total || uint64_t(prog) + 24 > total || uoff > total ||
            !usz || usz > total - uoff || usz % 16)
            continue;
        char label[64];
        std::snprintf(label, sizeof label, "EBOOT@%08zx", o);
        rsx_shader_precompile_add_flags(label, &b[o], total, RSX_PRECOMPILE_2D);
        n++;
        o += (total - 4) & ~size_t(3);   /* with the loop's += 4: to the program's end */
    }
    return n;
}

}  // namespace

/* For the wrapper UI: 1 while the startup work runs, with its progress (0..1)
 * and a line saying what it is doing. */
extern "C" int des_prework_status(float* fraction, char* text, int cap)
{
    const int st = s_state.load();
    if (st != 1 && st != 2) return 0;
    if (st == 1) {
        if (fraction) *fraction = 0.0f;
        if (text && cap > 0) std::snprintf(text, size_t(cap), "Reading the game's shaders");
        return 1;
    }
    rsx_shader_precompile_stats s;
    rsx_shader_precompile_get(&s);
    if (fraction) *fraction = rsx_shader_precompile_fraction();
    if (text && cap > 0)
        std::snprintf(text, size_t(cap), "%u shaders compiled, %u already prepared", s.built, s.cached);
    return 1;
}

/* For the wrapper UI: stop the startup work; what is left compiles as the game draws it. */
extern "C" void des_prework_skip(void)
{
    s_skip.store(true);
    rsx_shader_precompile_cancel();
}

namespace {

/* What a pass ended with (des_prework_shaders prints it). */
enum class Pass { Off, NoArchives, UpToDate, NotStarted, Skipped, Done, Failed };

#if defined(PS3RECOMP_RSX_HAL)
/* The target of the pass that runs (rsx_shader_precompile.h's RSX_PRECOMPILE_*). */
int s_target = RSX_PRECOMPILE_D3D12;

/* The pass's stamp file: today's for d3d12. Another target has its own in <cache>/gpu, and its stamp also names
 * the target and what its files depend on (its compiler, flags and text edits). */
std::string target_stamp(const char* cache, std::string& stamp)
{
    if (s_target == RSX_PRECOMPILE_D3D12) return std::string(cache) + kSep + "precompile.stamp";
    const rsx_shader_precompile_sink* k = rsx_shader_precompile_current();
    char tag[512] = "";
    if (k->tag) k->tag(tag, sizeof tag);
    stamp += std::string("target ") + k->name + " " + tag + "\n";
    const std::string dir = std::string(cache) + kSep + "gpu";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir + kSep + "precompile_" + k->name + ".stamp";
}
#endif

/* One pass over the shader archives of the game folder `root` and the
 * programs in its executable `elf` (may be empty), into the cache: at a start
 * (ps3_title_prework) or in setup (des_prework_shaders). `poll`, if given,
 * runs on this thread every 100 ms while the shaders compile. */
Pass run_pass(const char* root, const std::string& elf, bool force, void (*poll)(void), rsx_shader_precompile_stats& s)
{
#if defined(PS3RECOMP_RSX_HAL)
    const char* cache = s_target == RSX_PRECOMPILE_D3D12 ? rsx_d3d12_shader_cache_path()
                                                         : rsx_shader_precompile_current()->shader_cache_path();
#else
    const char* cache = rsx_d3d12_shader_cache_path();
#endif
    if (!cache || !cache[0] || !root || !root[0]) return Pass::Off;   /* the cache is off */
    const std::string dir = std::string(root) + kSep + "PS3_GAME" + kSep + "USRDIR" + kSep + "shader";
    s_state.store(1);
    struct Over { ~Over() { s_state.store(3); } } over;

    std::string stamp;
    const std::vector<std::string> names = list_archives(dir, stamp);
    if (elf.empty()) stamp += "eboot none\n";
    else stamp_file(stamp, "eboot", elf);
    stamp_file(stamp, "fp_variants", std::string(cache) + kSep + "fp_variants.txt");
#if defined(PS3RECOMP_RSX_HAL)
    const std::string stamp_path = target_stamp(cache, stamp);
#else
    const std::string stamp_path = std::string(cache) + kSep + "precompile.stamp";
#endif
    std::vector<uint8_t> old;
    if (names.empty()) {
        std::fprintf(stderr, "[des] shaders: no archives in %s; shaders compile as the game draws them\n", dir.c_str());
        return Pass::NoArchives;
    }
    /* The pipeline index, unless its pipelines are all built (pso_index.built says so). Over an up-to-date
     * cache the pass then does only the index's work. */
    std::string index_name;
    std::string index = read_index(index_name);
    if (!index.empty() && rsx_shader_precompile_index_built(index.data(), index.size())) index.clear();
    const bool up_to_date = !force && read_file(stamp_path, old) && std::string(old.begin(), old.end()) == stamp;
    const int rows = index.empty() ? 0 : rsx_shader_precompile_index(index.data(), index.size(), up_to_date);
    if (up_to_date && rows <= 0) {
        std::fprintf(stderr, "[des] shaders: cache up to date\n");
        return Pass::UpToDate;
    }
    if (rows > 0)
        std::fprintf(stderr, "[des] shaders: pipeline index %s, %d pipelines%s\n", index_name.c_str(), rows,
                     up_to_date ? " (the cache is up to date: building these only)" : "");

    if (!rsx_shader_precompile_kinds(kUnits, kAlts)) return Pass::NotStarted;
    int programs = 0;
    for (const std::string& n : names) {
        if (s_skip.load()) break;
        programs += queue_archive(dir + kSep + n, n);
    }
    if (!s_skip.load()) programs += queue_elf(elf);
    /* One processor is left for the window and its progress bar. */
    const unsigned cpus = std::thread::hardware_concurrency();
    const int threads = cpus > 2 ? int(cpus) - 1 : 1;
    if (s_skip.load() || rsx_shader_precompile_start(threads) != 0) {
        std::fprintf(stderr, "[des] shaders: precompile %s\n", s_skip.load() ? "skipped" : "did not start");
        return s_skip.load() ? Pass::Skipped : Pass::NotStarted;
    }
    s_state.store(2);
    if (poll) {
        for (rsx_shader_precompile_get(&s); s.running; rsx_shader_precompile_get(&s)) {
            poll();
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    rsx_shader_precompile_wait();
    rsx_shader_precompile_get(&s);
    std::fprintf(stderr,
                 "[des] shaders: %d programs, %u shaders -> %u compiled, %u already cached, %u failed%s; %.1f s on %d threads\n",
                 programs, s.shaders, s.built, s.cached, s.failed, s.cancelled ? " (skipped: the rest compile in play)" : "",
                 s.seconds, s.threads);
    if (rows > 0)
        std::fprintf(stderr, "[des] shaders: pipeline index: %u of %d pipelines have both programs in the game's files\n",
                     s.index_found, rows);
    /* A full pass is remembered. The game adds to fp_variants.txt as it
     * draws, which changes the stamp, so the next start builds the new
     * variants. */
    if (!up_to_date && !s.cancelled && !s.failed) {
        if (FILE* f = std::fopen(stamp_path.c_str(), "wb")) {
            std::fwrite(stamp.data(), 1, stamp.size(), f);
            std::fclose(f);
        }
    }
    return s.cancelled ? Pass::Skipped : up_to_date ? Pass::UpToDate : s.failed ? Pass::Failed : Pass::Done;
}

/* des_prework_shaders' progress line, when it changed: only once every
 * program is decompiled, when the count of shaders is final (it moves for
 * the first second, 15 to 3,763 before 3,603 on the US disc). */
void print_progress(void)
{
    static unsigned last_done = ~0u;
    rsx_shader_precompile_stats s;
    rsx_shader_precompile_get(&s);
    if (s.programs_done < s.programs || s.shaders_done == last_done) return;
    last_done = s.shaders_done;
    std::printf("shaders %u %u\n", s.shaders_done, s.shaders);
    std::fflush(stdout);
}

#if defined(PS3RECOMP_RSX_HAL)
/* A worse pass's result wins when the targets' results are put together. */
int pass_rank(Pass p)
{
    switch (p) {
    case Pass::Failed: return 6;
    case Pass::Skipped: return 5;
    case Pass::NotStarted: return 4;
    case Pass::Done: return 3;
    case Pass::UpToDate: return 2;
    case Pass::NoArchives: return 1;
    default: return 0;
    }
}

/* run_pass once for each target the runtime has, d3d12 first; a skip stops the rest. The result is the worst of
 * theirs, the counts and seconds their sums. */
Pass run_passes(const char* root, const std::string& elf, bool force, void (*poll)(void), rsx_shader_precompile_stats& s)
{
    Pass all = Pass::Off;
    for (int t = 0; t < RSX_PRECOMPILE_TARGETS && !s_skip.load(); ++t) {
        const rsx_shader_precompile_sink* k = rsx_shader_precompile_sink_of(t);
        if (!k) continue;
        rsx_shader_precompile_reset();
        rsx_shader_precompile_use(k);
        s_target = t;
        if (t != RSX_PRECOMPILE_D3D12) std::fprintf(stderr, "[des] shaders: the %s target\n", k->name);
        rsx_shader_precompile_stats one{};
        const Pass p = run_pass(root, elf, force, poll, one);
        s.built += one.built;
        s.cached += one.cached;
        s.failed += one.failed;
        s.seconds += one.seconds;
        if (pass_rank(p) > pass_rank(all)) all = p;
    }
    return all;
}
#endif

}  // namespace

/* The game's executable for the pass at a start: main's ELF, as
 * ps3_title_launch hands it to the boot. */
extern "C" void des_prework_elf(const char* path)
{
    s_elf = path ? path : "";
}

extern "C" void ps3_title_prework(void)
{
    des_ui_privacy_wait();   /* (the privacy note's question, when it is due: before anything goes online) */
    rsx_d3d12_pso_index_game(game_tag().c_str());   /* (boot_main has read PARAM.SFO) */
    const char* knob = std::getenv("DES_PRECOMPILE");
    if (knob && std::strcmp(knob, "0") == 0) return;
    rsx_shader_precompile_stats s{};
#if defined(PS3RECOMP_RSX_HAL)
    run_passes(ppu_vfs_root, s_elf, knob && same_nocase(knob, "force"), nullptr, s);
#else
    run_pass(ppu_vfs_root, s_elf, knob && same_nocase(knob, "force"), nullptr, s);
#endif
}

/* Setup's shaders phase (docs/52 section 5): tools/setup's pipeline.py runs
 * `des_host --shaders <game folder> <EBOOT.elf>` once the game is built (the
 * executable the game starts with, ready.txt's eboot), and this is all that
 * process does (ps3_title_launch): the same pass as at a start, with no
 * window, telling the engine how far it is on standard output, one line each:
 *   shaders <done> <total>     while the shaders compile, once the total is
 *                              known
 *   shaders <result> <built> <cached> <failed> <seconds>
 *                              at the end: done, up-to-date, none (no
 *                              archives, no cache, or no precompile on this
 *                              platform), skipped or failed
 * The cache and its stamp are the game's own, so its first start finds the
 * cache up to date. Returns the exit status: 1 when the pass did not finish
 * (the game then compiles what is left at its start, or as it draws). */
extern "C" int des_prework_shaders(const char* game, const char* eboot)
{
    rsx_shader_precompile_stats s{};
#if defined(PS3RECOMP_RSX_HAL)
    /* As below, one pass per target (on every OS: the spv target needs no FXC). */
    cellGame_init_from_paramsfo((std::string(game ? game : "") + kSep + "PS3_GAME" + kSep + "PARAM.SFO").c_str());
    const Pass p = run_passes(game, eboot ? eboot : "", false, print_progress, s);
#elif defined(_WIN32)
    /* The game's version, for its pipeline index: no boot reads PARAM.SFO in this process. */
    cellGame_init_from_paramsfo((std::string(game ? game : "") + kSep + "PS3_GAME" + kSep + "PARAM.SFO").c_str());
    const Pass p = run_pass(game, eboot ? eboot : "", false, print_progress, s);
#else
    (void)game;
    (void)eboot;
    (void)print_progress;
    const Pass p = Pass::Off;   /* the precompile is D3DCompile's: the shaders compile as the game draws them */
#endif
    static const char* const kResult[] = {"none", "none", "up-to-date", "failed", "skipped", "done", "failed"};
    std::printf("shaders %s %u %u %u %.1f\n", kResult[int(p)], s.built, s.cached, s.failed, s.seconds);
    std::fflush(stdout);
    return p == Pass::Done || p == Pass::UpToDate || p == Pass::Off || p == Pass::NoArchives ? 0 : 1;
}
