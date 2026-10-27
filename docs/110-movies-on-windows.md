# 110. Movies on Windows: FFmpeg beside des_host.exe

Date: 2026-10-26. Status: built in CI; checked in game (FFmpeg's decoder plays the title screen's advert, 30 frames
a second, and Media Foundation is never started). The runtime side is JonathanDC64/ps3recomp (sail_movie, FFmpeg on
Windows too). Background: docs/109 section 5.

## Short answer

- **Why:** Windows' own MPEG-2 decoder ("Microsoft MPEG Video Decoder MFT") starts only with the Microsoft Store's
  free MPEG-2 Video Extension; without it, activating it fails with `0xC004F011`. On such a PC every movie had no
  picture and ended after a moment: the opening, the endings (issue #202's "cutscenes skipped").
- **What ships:** the Windows zip has FFmpeg's decoder beside `des_host.exe`, as the Linux tarball does:
  - `avcodec-62.dll` and `avutil-60.dll`: FFmpeg 8.1.3 with only the MPEG-2 video decoder and its parser,
    LGPL 2.1 or later;
  - `licenses/ffmpeg/`: FFmpeg's licence, CREDITS, and a note with the source archive and the configure line.
  - Each release page has the source archive (`tools/release/third_party_sources.py`).
- **At run time:** each movie tries FFmpeg's decoder first, and uses Windows' own (Media Foundation) only when the
  DLLs can't be loaded. `PS3RECOMP_FFMPEG=0` skips FFmpeg.
- **Licence and patents:** the DLLs are loaded at run time and can be replaced with another build of the same
  major versions, which the LGPL asks for. MPEG-2 video's patents have expired.
- **Size:** about 1 MB.

## 1. How it is built (`.github/workflows/host.yml`)

- **`ffmpeg-windows`** (ubuntu-24.04):
  - It builds FFmpeg from the same pinned release archive as `host-linux`. The version and SHA-256 are in the
    workflow's env, once.
  - The options are the same: `--disable-everything`, then `--enable-decoder=mpeg2video --enable-parser=mpegvideo`.
    The step fails unless configure reports LGPL 2.1 or later.
  - It cross-compiles with mingw-w64 (`x86_64-w64-mingw32-gcc-win32`): Windows' own threads, so no winpthread DLL,
    and GCC's runtime linked in.
  - **Imports:** the DLLs may import only Windows' own DLLs, and the step fails on anything else. Their C runtime is
    `msvcrt.dll`, which every Windows has. Only FFmpeg's functions cross between them and the host, never a C
    runtime object.
  - **Cache:** keyed by the archive's SHA-256.
- **`host`:**
  - builds the runtime with `PS3RECOMP_FFMPEG_RUNTIME` and those headers (headers only: nothing links the DLLs);
  - copies the DLLs (under the majors the headers name) and `licenses/ffmpeg` beside the exe;
  - runs `check_host_imports.py` on the DLLs.
- **`package.py`:** takes the DLLs and `licenses/` from beside the exe into the zip and into `release-files.txt`,
  so an update swaps them with the rest of the program.
- **`release-check.yml`:** packages stand-ins and checks they are in the zip.

## 2. How to check it

- In `des.log`, at the first movie:
  - `[ffmpeg] libavcodec 62.x.y loaded`, then `[sail_movie] decoded N frames` every 300 frames while it plays, and
    `[sail_movie] stream read: N frames decoded` at its end;
  - `[cellSail] video GetFrame #N: ... new frames 30.0/s` says the game is shown the frames at the movie's rate;
  - `[sail_movie] no FFmpeg decoder: Windows' own (Media Foundation) instead` means the DLLs were not found beside
    the exe;
  - `[sail_movie] no Media Foundation on this PC ...` means Windows' own is missing too (section 3): the movies play
    their sound alone.
- **Developer builds** (`des_boot.exe`, a local `des_host.exe`) have no DLLs beside them and use Media Foundation.
  To try FFmpeg locally, copy `avcodec-62.dll` and `avutil-60.dll` from a CI run's `ffmpeg-windows-<sha>` artifact
  beside the exe. `tools/des` stages any DLL beside `DES_EXE` into the run folder.

## 3. Windows N editions

Windows 10 and 11's N and KN editions have no Media Foundation unless the Media Feature Pack is installed
(docs/45).
- **Before JonathanDC64/ps3recomp#143,** `des_host.exe` imported `MFPlat.DLL` directly, so it didn't start there at all.
- **Now**, the runtime looks up Media Foundation's six functions in System32's
  `mfplat.dll` the first time a movie needs Windows' decoder. `MFPlat.DLL` was the only import those editions lack,
  so the program starts there:
  - with FFmpeg's DLLs beside it (the package), the movies play as anywhere else;
  - without them (a source build), the log says
    `[sail_movie] no Media Foundation on this PC (a Windows N edition needs the Media Feature Pack), so no MPEG-2 decoder of Windows' own`
    once, and the movies play their sound alone.
- Checked on this PC with a scratch switch that loads a DLL name that doesn't exist; no N edition was at hand.
