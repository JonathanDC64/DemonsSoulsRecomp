# 87. Setup on Linux (design note)

Date: 2026-10-22. Status: design only. Nothing was built or run for it. It uses the docs, our source, LLVM's release pages and the GitHub API.

Update 2026-10-25: built. docs/106 says what was built and where it differs from this note.

Labels, as in the other notes:
- **Read**: seen in a doc, our source, a web page or the GitHub API.
- **Measured**: checked on this PC.
- **Inferred**: a conclusion that still needs a check.

## Short answer

- **The same shape as on Windows** (docs/52 Route B, docs/81). CI builds the host. The player's PC lifts the game and compiles only the game part, as `des_game.so`, with a compiler that setup downloads, pinned by SHA-256.
- **The compiler: LLVM's own Linux archive, trimmed in CI**, as docs/86 does for Windows: clang, ld.lld and clang's headers, 22.1.8 on both platforms. Not the distro's clang: it differs on every PC, cannot be pinned, and often is not installed.
- **The split needs an ELF version.** Today it is Windows only (docs/57 section 6.1): PE/COFF thunks and `LoadLibrary`. On Linux it needs `dlopen`, the thunks with ELF directives, hidden symbols, and no thread-locals in the `.so`.
- **Proton stays the cheap route** (docs/75): the Windows build and its Windows setup, under Proton. It needs no Linux setup code at all.
- **Order:** Windows milestone 2 first (most of it is shared), then the split on ELF, the Linux host in CI, the Linux compiler job, and setup's Linux parts. Section 6 has the list.

## 1. Each Windows piece, and its Linux equivalent

| Piece | Windows (docs/52, docs/81, docs/86) | Linux |
|---|---|---|
| The host | `des_host.exe` from CI: clang-cl, the MSVC C++ library and VC runtime built in, the system UCRT | `des_host` from CI: clang, libstdc++ built in, the system's glibc (section 2) |
| The game part | `des_game.dll`, built on the player's PC | `des_game.so`, built on the player's PC (section 3) |
| Loading it | `LoadLibrary`, PE/COFF thunks | `dlopen`, ELF thunks (section 4) |
| Compiler | the trimmed zip from `clang+llvm-22.1.8-x86_64-pc-windows-msvc.tar.xz` | a trimmed tar.xz from `LLVM-22.1.8-Linux-X64.tar.xz` (section 3.1) |
| Linker | `lld-link.exe` | `ld.lld` (the same lld program, ELF flavour) |
| Python for the lifters | python.org's embeddable 3.13.9 zip | no such package for Linux; a pinned standalone build, or the system's `python3` (section 3.2) |
| Downloads | `fetch.py`: zip only | `fetch.py` with a tar branch (section 3.1) |
| Data folder | `%LOCALAPPDATA%\DemonsSoulsRecomp` | `$XDG_DATA_HOME/DemonsSoulsRecomp`, else `~/.local/share/...` (docs/76) |
| Kill the compilers if setup dies | a job object with kill-on-close | a process group, plus `PR_SET_PDEATHSIG` on each child (inferred) |
| Keep the PC awake | a power request | a logind inhibitor (`systemd-inhibit`, or its D-Bus call) (inferred) |
| Screens | RmlUi in the host | the same RmlUi screens, on the host's D3D12-on-vkd3d renderer (docs/76) |
| Release package | a zip | an AppImage of the host (docs/76's `appimage.sh`, without the game files) |

## 2. The host, built in CI for Linux

What it links today (read, docs/76):
- **vkd3d 2.1**: Wine's D3D12 on Vulkan, built from its release tarball. It is LGPL-2.1, linked as shared libraries.
- **SDL2**: the window, audio, controllers and keyboard.
- **The Vulkan loader**, and through it the GPU's driver.
- **FFmpeg's libavcodec**: loaded only when a movie starts, at run time. Without it a movie ends at once.

Proposal:
- **glibc: the system's, always.** A program built against a newer glibc does not start on an older one (docs/76: "the system's glibc must be at least as new as the build machine's"). So CI builds on an old base:
  - Valve's Steam Runtime 3 "sniper" SDK image, `registry.gitlab.steamos.cloud/steamrt/sniper/sdk`. A developer's build notes give its floor as "glibc 2.31+, 64-bit only" (read). Steam itself needs glibc 2.31 or newer since August 2025 (read, a news article). Its Debian base: not confirmed here.
  - Or the `ubuntu-22.04` runner, with glibc 2.35 (inferred). LLVM builds its Linux release there "to make the built binaries more portable (eg functional against older glibc)" (read, `release-binaries.yml`).
  - Recommendation: sniper. Most Linux players run Steam, and the Steam Deck is the main target. The compiler sets its own floor anyway (section 3.1).
- **The C++ library built in**: `-static-libstdc++ -static-libgcc`, so the host does not need a newer libstdc++ than the player's (inferred). GCC's runtime library exception allows this (inferred). It is the Linux form of building in the VC runtime (docs/81).
- **vkd3d: shared, beside the host.** The AppImage already does this, with a notice saying where the source is and that the libraries can be replaced (docs/76). That is the simplest way to meet the LGPL. CI already builds vkd3d 2.1 from its tarball, pinned by SHA-256 (`ci.yml`, `ui-linux`).
- **SDL2, the Vulkan loader, FFmpeg: the system's.**
  - The Vulkan loader must come from the system, because it finds the system's drivers (inferred).
  - SDL2's library name (`libSDL2-2.0.so.0`) has stayed the same for years (inferred). docs/76's AppImage takes it from the system too.
  - FFmpeg stays optional, as today.
- **CI checks the result.**
  - Every `NEEDED` entry must be on an allow-list: glibc's libraries, SDL2, Vulkan and our vkd3d.
  - The highest `GLIBC_x.y` symbol version must be at or below the floor.
- **No lift in CI.** As on Windows, this needs docs/57 section 6.2's header split: the host must build without a lift.

## 3. The game part on the player's PC

The same module as Windows milestone 2 (docs/57 section 6.2, docs/52 section 7.8), built for ELF: no C library, no system headers, only clang's own headers and ours:

```
clang --target=x86_64-unknown-linux-gnu -c -O2 -fPIC -fvisibility=hidden -nostdlibinc -I<our headers> \
      -fno-stack-protector -fno-math-errno -fwrapv -msse4.1 -funwind-tables ...
ld.lld -shared -nostdlib -z defs -z now --build-id=sha1 --version-script=des_game.ver -Map=des_game.map \
       -o des_game.so <objects>
```

- **No C library on Linux too.** `-nostdlibinc` keeps the system's headers out, and a small folder of our own headers declares what the lift uses. Then the player needs no libc headers, start files or `libc.so` linker script. Those come from `libc6-dev` or `glibc-devel`, which many PCs do not have, and a Steam Deck does not (inferred). The `.so` then needs nothing from glibc, because every outside call goes through the host (docs/57 section 6.2's plan).
- **Not `-ffreestanding`.** It also turns off clang's builtins: a 16-byte `memcpy` becomes a call, and `sqrt` loses its inline `sqrtsd` (measured on Windows, 10-22, clang 22.1.8; `-fbuiltin` after it does not undo it). The lift has tens of thousands of small `memcpy`s, so the module stays a hosted compile, and our headers supply what `-ffreestanding` would have avoided needing (for example the `<stdlib.h>` that clang's `mm_malloc.h` includes).
- **One export.** `des_game.ver` exports `des_game_init` and hides everything else (section 4).
- **Relocations.** The function table holds about 94,000 pointers (docs/57: 94,017 functions). In a `.so` each is a relative relocation that the loader applies at `dlopen`: a few milliseconds (inferred). The Windows DLL has base relocations for the same reason. `-z pack-relative-relocs` would shrink them, but needs glibc 2.36 at run time (inferred), so not with a 2.31 floor.
- **RAM.** One `/O2` chunk peaks near 3 GB (docs/75). A Deck has 16 GB, shared with its GPU, so the job count must follow free RAM (docs/52 section 4.4). `/proc/meminfo`'s `MemAvailable` gives it.

### 3.1 The compiler: LLVM's archive, or the distro's clang

LLVM's Linux archive for 22.1.8 (read, the GitHub API, 2026-10-22):
- `LLVM-22.1.8-Linux-X64.tar.xz`, 1,938,859,476 bytes, SHA-256 `df0e1ecf16caf3489a272a5eea4eec9b0d82878f6477fa309504f918a0006384`.
- Checked the same ways as the Windows one:
  - its `.jsonl` attestation names that SHA-256 and the same signer, `release-binaries.yml` at `refs/tags/llvmorg-22.1.8` (read);
  - its `.sig` is by the same GPG key, `FFB3 3689 ... 64CA CBA5` (measured with `gpg --list-packets`).
- Built with `clang/cmake/caches/Release.cmake` on an `ubuntu-22.04` runner: PGO, ThinLTO, BOLT for clang, and zstd built in (read). LLVM's own libc++ is not linked in (`LLVM_RELEASE_ENABLE_LINK_LOCAL_RUNTIMES` is off), so clang probably needs the system's libstdc++ (inferred).
- Which shared libraries `clang` and `ld.lld` need (zlib? libxml2?): not confirmed. The CI job lists them with `ldd` and fails on any outside an allow-list.

| | LLVM's archive, trimmed in CI | The distro's clang |
|---|---|---|
| The same compiler on every PC | yes: one file, pinned by SHA-256 | no: each distro has its own version and patches. Debian 13's `clang` is 19 (read, packages.debian.org); others differ. |
| The same as Windows | yes, 22.1.8 | no |
| Pinning | size, SHA-256, LLVM's attestation and GPG signature, as in docs/86 | a package name only |
| Checking the built `.so` against a published hash (docs/45 section 4.5) | possible | not possible |
| Download | not measured. The source archive is 1.9 GB. The trimmed set might be 50 to 80 MB compressed (inferred from Windows's 49 to 69 MB). | none |
| Install | none: it lives in the data folder | the player installs it with root. On a Deck that means unlocking the read-only system first (inferred). |
| Runs on | glibc 2.35 or newer (inferred, from the build runner), and the system's libstdc++ | wherever the distro runs |
| Risk | PCs older than glibc 2.35 cannot run it | lifter output that one compiler version mishandles. The rotate-by-zero bug (docs/08, fix 1) was undefined behaviour that clang miscompiled. Each distro version is a new compiler to trust. |

**Recommendation: LLVM's archive.** For PCs older than glibc 2.35, later: a clang built in CI for an older base, or a static one (docs/45's "smaller custom LLVM"). The distro's clang can stay a developer option, outside setup.

**Trimming it, and what differs from Windows:**
- **Links.** Unix install trees use links: `clang` points to `clang-22`, and `ld.lld` to `lld` (inferred). The packer stores the real files under the names setup calls, `bin/clang` and `bin/ld.lld`, so the archive holds no links. lld picks its flavour from its own name (inferred), and clang finds its headers from its own folder.
- **The executable bit.** Python's `zipfile` never restores file modes: `_extract_member` in CPython 3.13.9 opens the file and copies the data, with no `chmod` (read). So a zip would unpack a `clang` that cannot run. Two ways:
  - **tar.xz, unpacked by `tarfile` with `filter="data"`** (recommended; smaller too). The filter is in Python 3.12 and later (read, Python's docs). It refuses absolute paths, paths that leave the folder, links that point outside, and device files. It keeps the owner's execute bit.
  - Or a zip, plus a `chmod` from each entry's `external_attr` after unpacking.
- **`fetch.py` gains a tar branch.** `ensure` today accepts only `.zip` (read). Tests on made-up archives: the execute bit is kept; an absolute path, a `..` path, an outside link and a device file are each refused.
- **The CI job**, like `llvm-archive.yml`, on the `ubuntu-22.04` runner (the floor):
  - download the archive, and check its size and SHA-256, the attestation and the GPG signature;
  - keep `clang`, `ld.lld` and `lib/clang/22/include`, plus the licence files;
  - pack a tar.xz with fixed times, owners and modes;
  - smoke test: compile and link a `.so` with only those files, check it has no `NEEDED` entries and exports only its functions, load it with `ctypes` and call it (as docs/86's smoke test does on Windows);
  - list `ldd bin/clang` and `ldd bin/ld.lld`;
  - upload to the same draft release as the Windows zip.

### 3.2 Python on Linux

python.org publishes the embeddable Python only for Windows (inferred). Two options:
- **A pinned standalone build**, from the python-build-standalone project (read, its GitHub releases). For example, release 20261021 has `cpython-3.13.17+20261021-x86_64-unknown-linux-gnu-install_only_stripped.tar.gz`, 35,356,674 bytes; the API lists its SHA-256. It can be pinned like the others, from the same 3.13 line as Windows. Its glibc floor: not checked.
- **The system's `python3`.** Most distros have one (inferred), but the version varies, and a distro may lack modules (Debian splits out `venv` and others). The lifters' oldest working version is not known.

Recommendation: the pinned standalone build, for the same reasons as the compiler.

## 4. What the split needs on Linux

docs/57's design carries over. The ABI (`game_module.h`), the host's pointers to the DLL's globals (`game_split.h`), `des_game_init` and the two lists all stay. What changes:

- **Loading.** `dlopen(path, RTLD_NOW | RTLD_LOCAL)`, `dlsym(h, "des_game_init")`, and `dlerror()` for the message. This is one small function per platform in `game_host.c` (open, find a symbol, the error text), and its callers have no `#ifdef`. That is the rule the port already follows: portable calls, and a per-platform function only where a fact differs (docs/76, "One code path").
- **The thunks.** The jump is the same, `jmp *slot(%rip)`. The directives around it differ: ELF uses `.globl`, `.hidden`, `.type name,@function` and `.size` where COFF has its own. `PS3_GAME_THUNK` gets one form per object format. On Linux clang reads module assembly as AT&T anyway, so the `.att_syntax prefix` line stays harmless (inferred).
- **Hidden symbols.**
  - In ELF, a `.so`'s default-visibility symbol can be overridden by the exe's symbol of the same name (inferred, standard ELF). Here both define `vm_read32`: the runtime in the host, a thunk in the `.so`.
  - So the `.so` is built with `-fvisibility=hidden`. The forced-include header adds `#pragma GCC visibility push(hidden)`, so the lift's `extern` declarations are hidden too. The version script exports only `des_game_init`.
  - It also keeps the lift's fast paths as they are. A hidden global is read relative to the instruction pointer, as on Windows. A default-visibility one would go through the GOT: one more load on every memory access the inline accessors make (inferred).
- **An ELF-only shortcut, not taken.** ELF could leave the 47 functions undefined and let the loader bind them to the exe's own (`--export-dynamic-symbol`). That drops the thunks on Linux, but makes a second design to keep working. Keep the thunks: one design on both.
- **Thread-locals.** The two trampoline slots, `g_trampoline_fn` and `g_spu_trampoline_fn`, are thread-locals in the module (docs/57 section 3.4).
  - In a `dlopen`ed `.so`, a thread-local uses the general-dynamic model by default: each access calls `__tls_get_addr` (inferred). That is slow on a hot path: the runtime reads and clears `g_trampoline_fn` after every call it makes (docs/52 section 7.8). It is also a glibc symbol, which the freestanding `.so` cannot import.
  - The initial-exec model works in a `dlopen`ed `.so` only while glibc's small spare static TLS lasts (inferred).
  - **Recommendation:** move both slots into `ppu_context` and `spu_context` (docs/57 decision 1). The Windows DLL needs the same change to drop the C runtime's TLS (docs/57 section 6.2), so one change fixes both platforms.
- **No static constructors.** As on Windows: the build turns the SPU registration into a plain call (docs/57 section 3.1).
- **Calling convention.** Both sides use the System V ABI, and the runtime calls are `extern "C"` (docs/75). Nothing to do.
- **Crash reports.** The host's signal handler already writes a backtrace (docs/76). `-funwind-tables` lets it walk lifted frames (inferred). `ld.lld -Map` gives `des_game.map` to symbolize them.
- **Frame rate.** docs/57's A/B was Windows only. The split needs one on Linux too, on a real Linux PC: WSL with dzn sits at 60 Hz and says little (docs/76).

## 5. Proton instead (docs/75)

- **What:** run the Windows build under Proton or Wine, as a non-Steam game. Setup is the Windows one.
- **Setup under Wine.** The host, `clang.exe`, `lld-link.exe` and the embeddable Python are ordinary Win32 programs (docs/75). Still to check there: the job object's kill-on-close, Python's worker processes, and RAM during the compile (docs/75).
- **For:** no Linux setup code, no Linux CI host, one release for both.
- **Against:**
  - the risks docs/75 lists: Wine's HLSL compiler, movies through Media Foundation, and the depth-format substitution on AMD;
  - players need Steam or Wine;
  - the native build already runs (docs/76).
- **Recommendation:** Proton is the first route for players who already use it, because it costs nothing extra once Windows setup exists. Build the native setup after the split works on ELF.

## 6. Steps, in order

1. **Windows milestone 2** (docs/57 section 6.2): the freestanding module, the trampolines moved into the contexts, and the header split. Linux needs all three.
2. **The split on ELF**:
   - the `dlopen` loader, the ELF thunk macro, hidden visibility and the version script;
   - the Linux side of `DES_SPLIT` in CMake;
   - boot it in WSL, then run the frame-rate A/B on a real Linux PC.
3. **The Linux host in CI**:
   - the sniper SDK image (or `ubuntu-22.04`), vkd3d from its pinned tarball, and libstdc++ built in;
   - check the `NEEDED` list and the `GLIBC_` symbol versions.
4. **The Linux compiler job**, beside `llvm-archive.yml` (section 3.1).
5. **Python**: pin a standalone build, or decide on the system's.
6. **`fetch.py`'s tar branch** and its tests.
7. **Setup's Linux parts**:
   - a process group and `PR_SET_PDEATHSIG` for kill-on-close;
   - the job count from `MemAvailable`;
   - a logind inhibitor while building;
   - the XDG data folder.
8. **The release AppImage**: the host only, with no game files.
9. **Tests**: a Steam Deck, Ubuntu 24.04, and one rolling distro (Arch or Fedora), each with and without Steam.

## Open questions

1. **The glibc floor**: 2.31 (sniper) or 2.35 (`ubuntu-22.04`)? LLVM's Linux clang probably needs 2.35 (inferred), so the compiler sets the floor unless we build our own.
2. **SDL2**: from the system, or a pinned copy beside the host?
3. **Python**: a standalone build, or the system's `python3`?
4. **The distro's clang**: keep it as an unsupported developer option, or leave it out?
5. **arm64 Linux**: out of scope here (docs/46 B4).

## Sources

- LLVM 22.1.8's assets and attestations: `https://api.github.com/repos/llvm/llvm-project/releases/tags/llvmorg-22.1.8` and `.../attestations/sha256:df0e1ecf16caf3489a272a5eea4eec9b0d82878f6477fa309504f918a0006384`, read 2026-10-22.
- LLVM's source at `llvmorg-22.1.8`: `.github/workflows/release-binaries.yml`, `clang/cmake/caches/Release.cmake`.
- CPython v3.13.9, `Lib/zipfile/__init__.py` (`_extract_member`). Python 3.13 docs, `tarfile`, "Extraction filters": `https://docs.python.org/3.13/library/tarfile.html`.
- Debian's `clang` package in trixie: `https://packages.debian.org/trixie/clang`.
- python-build-standalone releases: `https://github.com/astral-sh/python-build-standalone/releases` (release 20261021, through the GitHub API).
- Steam Runtime: a developer's build notes, `https://icculus.org/finger/flibitijibibo` (the sniper SDK image, "glibc 2.31+"); Steam's glibc 2.31 floor: `https://linuxiac.com/steam-will-stop-working-on-outdated-linux-systems-this-august/`.
- docs/45, docs/52, docs/57, docs/75, docs/76, docs/81, docs/86.
