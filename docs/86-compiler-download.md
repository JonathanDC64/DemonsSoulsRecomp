# 86. The compiler setup downloads

Date: 2026-10-22. Status: the workflow ran once on GitHub (run 39870384825, LZMA, upload on) and passed. It attached `llvm-win64-22.1.8-1.zip` (49,496,165 bytes) and its JSON to the draft release `llvm-22.1.8-1`, and `toolchain.json` pins that zip (section "How the sums get into `toolchain.json`", step 3). LLVM's 862 MB archive was downloaded only on the runner, never here. The packing is tested on a made-up archive (`tools/setup/tests/test_llvm_pack.py`), and the smoke test ran against this PC's own LLVM 22.1.8 install and then on the runner. The rest comes from web pages, the GitHub API and LLVM's source.

Labels, as in the other notes:
- **Read**: seen on a web page, in the GitHub API or in our source.
- **Measured**: checked on this PC.
- **Inferred**: a conclusion that still needs a check.

Files: `.github/workflows/llvm-archive.yml` (the workflow), `.github/scripts/llvm_pack.py` (its packing and smoke test), the `llvm-win64` entry in `tools/setup/des_setup/toolchain.json` (pinned 10-22), and the tests in `tools/setup/tests/` (`test_llvm_pack.py`, and the manifest tests in `test_setup.py`).

## Short answer

- Setup's compiler is a zip that CI makes from LLVM's own 22.1.8 Windows archive. It holds `clang.exe`, `lld-link.exe`, clang's headers and the licence files. Nothing else.
- A workflow started by hand does it on a Windows runner. It downloads LLVM's archive there and checks it three ways: size and SHA-256, LLVM's build attestation, and the release manager's GPG signature. Then it packs the zip, builds a tiny DLL with only the zip's files, and uploads the zip to a draft release.
- Setup downloads that zip like the Python zip: pinned by size and SHA-256 in `toolchain.json` (docs/81, section 3).
- Size: about 49 MB with LZMA, the default (docs/45 measured 48.7 MB with xz and 68.8 MB as a deflate zip for the same files). Setup's Python reads both kinds.
- Setup runs it as `clang.exe --driver-mode=cl`, with CMake's own clang-cl commands for `des_game` (docs/81 section 6, step 3). It gives the same objects as `clang-cl.exe` (measured).
- A decision for you: which release carries the zip (Open questions).

## LLVM's archive

From the GitHub API, release `llvmorg-22.1.8`, published 2026-06-16 (read, 2026-10-22):

| Asset | Size (bytes) | SHA-256 (the API's `digest`) |
|---|---|---|
| `clang+llvm-22.1.8-x86_64-pc-windows-msvc.tar.xz` | 862,053,924 | `d96c2cc1736f4eb7fa43cb9bbdf56d93551a9ae0a9aadb9c99c3c3b2b712a234` |
| its `.jsonl` (attestation) | 11,207 | `cf5e958a316021d1c1f66b9a9ac10367aa471adee01b3d6a3f8f27af0400d4be` |
| its `.sig` (GPG) | 119 | `1fdf8cb3e730d7e2769b0301a24f059d6845e1023732e231092f7d00d502fc09` |
| `LLVM-22.1.8-win64.exe` (installer) | 455,545,840 | `16e5709785fef73c854646241c4a92c5cd574318d1b33c63330dd7721903e55c` |

The installer is out: it installs. The archive is "the contents of the installer, plus libraries and tools not normally used in a toolchain" (read, the release text).

**How LLVM says to check it** (read, the release text): a `.sig` file is a GPG signature by a release manager, checked against `releases.llvm.org/release-keys.asc`; a `.jsonl` file is checked with `gh attestation verify --repo llvm/llvm-project`.

**The attestation** (read, the GitHub API's attestation for the archive's digest):
- A Sigstore bundle with SLSA provenance v1, from Sigstore's public instance.
- Its subjects are the installer and the archive, each with the SHA-256 above. The `.jsonl` beside the installer is the same file.
- It was signed by `.github/workflows/release-binaries.yml` at `refs/tags/llvmorg-22.1.8`, called from `release-tasks.yml`.
- The signing job runs on GitHub's `ubuntu-24.04` runner (read, `release-binaries.yml`). The build itself runs on larger runners that LLVM picks.

**The signature** (measured: `gpg --list-packets` on the 119-byte `.sig`, and `gpg --show-keys` on `release-keys.asc`):
- EdDSA with SHA-512, made 2026-06-16.
- Key fingerprint `FFB3 3689 80F3 E6BB 5737 145A 316C 56D0 64CA CBA5`: an ed25519 key from 2025-09-15, with no expiry, listed in `release-keys.asc`.
- Whether it is a good signature over the archive: not checked here, because that needs the archive. The workflow checks it.

**The layout** (read, `llvm/utils/release/build_llvm_release.bat` at the tag): the script runs `ninja install` with `LLVM_INSTALL_TOOLCHAIN_ONLY=OFF` into a folder named `clang+llvm-22.1.8-x86_64-pc-windows-msvc`, then 7-Zip makes the tar and the xz. So the archive has that one top folder (inferred; `llvm_pack.py` checks it).

The files we keep, by their place in LLVM's install tree (measured on this PC's 22.1.8 install, docs/52 section 6.1):
- `bin/clang.exe`, 104,745,472 bytes;
- `bin/lld-link.exe`, 71,456,768 bytes;
- `lib/clang/22/include`: clang's own headers, 16 MB, 249 entries.

That these are the same bytes in the archive: not confirmed. The installer and the archive come from one build job and one build folder (read: one attestation names both, and the script makes the archive from the installer's build), so it is likely (inferred). The job's JSON records each kept file's SHA-256.

**Does `clang.exe` need a DLL beside it?** Not for the installed copy. Measured today with the job's own PE reader:
- `clang.exe` imports `KERNEL32`, `ADVAPI32`, `ntdll`, `VERSION` and `OLEAUT32`, and delay-loads `ole32` and `SHELL32`.
- `lld-link.exe` imports the same without `VERSION`.
- All of them are part of Windows.

For the archive's copy: not confirmed. The job checks it two ways (section "The workflow"). The archive's `bin/` probably also holds `LLVM-C.dll` (inferred: the script sets `LLVM_BUILD_LLVM_C_DYLIB=ON`). Neither exe imports it.

## What is in the zip, and why

| In the zip | Why |
|---|---|
| `bin/clang.exe` | The compiler. Setup calls it as `clang --driver-mode=cl`, which is clang-cl (docs/81 section 6, step 3). |
| `bin/lld-link.exe` | The linker for `des_game.dll` (`/dll /noentry /nodefaultlib`, docs/57 section 6.2). |
| `lib/clang/22/include/` | clang's own headers: `stdint.h`, `stddef.h`, `stdatomic.h`, `intrin.h`, `smmintrin.h` and the rest. The game part uses no other headers (docs/52 section 7.8). |
| `licenses/` | LLVM's, clang's and lld's `LICENSE.TXT`, and libxml2's notice (section "Licence"). |
| `README.txt` | Where the files came from (URL and SHA-256), and that they are unchanged. |

Left out, on purpose:
- **`clang-cl.exe`**: the same bytes as `clang.exe` (measured, docs/45). Setup runs `clang.exe --driver-mode=cl` instead, which gives the same objects (measured on an SPU image, the libsre glue and `des_game_module.cpp`).
- **`lld.exe`, `ld.lld.exe`**: the same file as `lld-link.exe` (measured, docs/52).
- **compiler-rt**: not needed (docs/52 section 6.1). See Open questions for the one case that would need it.
- **Everything else**: about 2.9 GB of tools, libraries and headers the game part never uses (docs/45).

The paths stay as LLVM has them. clang finds its headers at `../lib/clang/22` from its own folder, so setup needs no `-resource-dir` flag.

**Zip or tar.xz.** docs/45 measured the same files at 68.8 MB as a deflate zip and 48.7 MB as tar.xz: xz is 20 MB (29%) smaller.
- Setup's Python can read both. The embeddable Python 3.13.9 has `_lzma.pyd`, `lzma.pyc` and `tarfile.pyc` (measured on the pinned zip, SHA-256 `91d828c2...`).
- There is a third way that keeps setup's code as it is: a zip whose entries use LZMA (`zipfile.ZIP_LZMA`). `fetch.unpack` reads it with no change. Its size should be close to xz's, because two exes make up most of it (inferred, not measured on the real files).
- Measured on a made-up archive (`llvm_pack.py`'s self-test): both kinds come out byte for byte the same when packed twice, and both unpack through setup's own `fetch.unpack`.
- Explorer opens deflate zips but not LZMA ones (inferred). Players never need to open it.

The workflow has a `compression` input for this. It defaults to LZMA, since the smaller download was the point of the trimmed archive (docs/81).

## The workflow (`llvm-archive.yml`)

- **Start.** `workflow_dispatch` only, from the Actions tab. It needs write access to the repository, and the file must be on the default branch (read, GitHub's docs).
- **Inputs.**
  - `release_tag`: the draft release to attach to.
  - `package_revision`: the `1` in `22.1.8-1`.
  - `compression`: lzma (the default) or deflate.
  - `upload`: off for a dry run that only reports.
- **Runner.** `windows-2025`, because the smoke test runs the exes. Python 3.13.9, the embeddable Python's version, so the zip writer and its zlib match.
- **Pinned.** Actions by commit SHA, with the SHAs the other workflows use (docs/54). In the workflow's `env`: the archive's URL, size and SHA-256; the `.jsonl` and `.sig` sizes and sums; the signer workflow and its tag; the GPG key fingerprint; the licence files' URLs and sums.
- **Permissions.** `contents: write`, for the upload. Until now only `release.yml` could write (docs/54 section 8.1). This one is the second, and it can only be started by hand.

Steps:
1. **Inputs.** The tag must be a plain tag name, and the revision a whole number. Inputs reach the scripts as environment variables, never pasted into them.
2. **The release.** With `upload` on: the release must exist and be a draft, and it must not have this zip already. A changed package needs a new revision, so a pinned sum never changes under setup. The check runs first, so a wrong tag fails in seconds.
3. **Download, on the runner.** The archive, its `.jsonl` and its `.sig`, each checked by size and SHA-256.
4. **The attestation.** `gh attestation verify` with the pinned bundle, `--repo llvm/llvm-project`, `--signer-workflow llvm/llvm-project/.github/workflows/release-binaries.yml`, `--source-ref refs/tags/llvmorg-22.1.8` and `--deny-self-hosted-runners`.
5. **The signature.** The workflow imports `release-keys.asc` into a fresh GPG home and checks the signature. It passes only if GPG reports `VALIDSIG` for the pinned fingerprint. The keyring file itself is not pinned, because it grows with each new release manager.
6. **Licence files.** Fetched from LLVM's source at the tag and from libxml2's at v2.9.12, each checked by SHA-256.
7. **Pack** (`llvm_pack.py pack`).
   - It checks the archive's SHA-256 again, so it never packs an unchecked file, even when run by hand.
   - It reads the tar.xz once, as a stream, and keeps only the files above.
   - It refuses an archive whose kept files are not plain files (links), or whose entries leave the top folder. It also refuses one where a kept file or a basic header is missing.
   - It writes the zip in a fixed way: sorted entries, one date (2026-06-16, LLVM's release day) and fixed modes, as `tools/release/package.py` does. So the same archive gives the same zip.
   - It writes `llvm-win64-22.1.8-<rev>.json`: the `toolchain.json` entry, the source archive, and each file's size and SHA-256.
8. **Unpack** with setup's own `fetch.unpack`, from the checkout.
9. **Smoke test** (`llvm_pack.py smoke`), with only Windows' own folders on `PATH`, and no `INCLUDE` or `LIB`:
   - it reads the DLLs `clang.exe` and `lld-link.exe` import, and fails if one is not part of Windows;
   - `clang --version` and `lld-link --version` must say 22.1.8;
   - clang's resource folder, and every folder in its header search list, must be inside the unpacked zip;
   - it compiles a small C file (`stdint.h`, `stddef.h`, `stdatomic.h`, `intrin.h`, SSE4.1 from `smmintrin.h`, a double, `_fltused` defined in the file) with `-ffreestanding -nostdlibinc -fno-stack-protector -msse4.1`;
   - it links it with `lld-link /dll /noentry /nodefaultlib /lldignoreenv`;
   - the DLL must be x64, have no entry point, export its functions and import nothing;
   - it loads the DLL and checks six calls;
   - then the same in cl mode, as setup runs clang: `clang.exe --driver-mode=cl /X`, its header search list inside the zip, a DLL with no imports, two calls. `/X` matters: without it clang-cl finds Visual Studio's headers through the registry, even with no `INCLUDE` (measured). The smoke file is freestanding here, because `mm_malloc.h`, which `intrin.h` reaches, wants `<stdlib.h>` in hosted mode. The game part stays hosted, with its own small headers, because `-ffreestanding` also turns `memcpy` and `sqrt` into calls (measured; docs/52 section 7.8).

   Run on 10-22 against this PC's LLVM 22.1.8 install, in place of the zip: every check passed. clang.exe imports KERNEL32, ADVAPI32, ntdll, VERSION and OLEAUT32 and delay-loads ole32 and SHELL32; lld-link.exe the same without VERSION.
10. **Authenticode.** It reports whether `clang.exe` and `lld-link.exe` carry a signature, which docs/45 left open (question 10). It does not fail the job.
11. **Upload** with `gh release upload` (no `--clobber`). GitHub computes each asset's SHA-256 on upload. The job reads it back and stops if it differs from its own.

The summary shows the zip's size and SHA-256, what LLVM's archive held (its licence files and the DLLs in `bin/`), the smoke test's results, and the `toolchain.json` entry ready to paste.

Time: about 5 to 10 minutes. The download is 862 MB, and the stream is decompressed once (inferred).

## How the sums get into `toolchain.json`

1. Make the draft release, if it is not there yet: `gh release create <tag> --draft --verify-tag`. The job never makes releases or tags.
2. Run the workflow once with `upload` off, for each compression, to compare sizes. Then run it with `upload` on.
3. Copy the `entry` object from the summary (or from the JSON asset) into `toolchain.json`, in a pull request. Until then the entry stays with `size` and `sha256` null, and `fetch.py` refuses it ("not pinned yet").
4. Publish the release. A draft's assets are not public, so players can only download the zip after that.
5. Run `python -m des_setup fetch llvm-win64` once on a PC. It downloads the zip, checks the size and SHA-256, and unpacks it. Say so in the entry's `checked` text.

**Done for `llvm-22.1.8-1`** (10-22):
- Steps 1 and 3 done; step 2 was skipped (one run, LZMA). The release is still a draft: step 4 is the maintainer's.
- Before step 4, the zip was downloaded from the draft with `gh release download`: size and SHA-256 equal the JSON's and the entry's (measured). Put at `archive_path` in a scratch cache, `fetch.ensure` took it without a download, unpacked it and wrote the stamp; `fetch.unpacked` is true (measured).
- Step 5, the plain HTTPS download, waits for the release to be published.
- What the run reported: `clang.exe` and `lld-link.exe` carry no Authenticode signature. Neither do the installer's (measured, `Get-AuthenticodeSignature`).

The tests that guard the entry (`test_setup.py`): every real entry is pinned or refused; the compiler entry's URL ends in `<name>-<version>.zip`; once pinned, its size is between 30 and 150 MB; and an LZMA zip unpacks through `fetch.ensure`. The entry's URL names the recommended release tag, `llvm-22.1.8-1` (Open questions).

`python -m des_setup tools` used to format the size with `:,`, which crashes on a null size. It now prints "size not set" for the unpinned entry.

**Where the zip lives.** A version release cannot carry its own sums: the version tag fixes `toolchain.json` before the job can run on that draft. Two ways:
- (a) **A release of its own** (recommended), for example tag `llvm-22.1.8-1`. Its URL is known before the job runs, every port version can point at it, and it changes only when LLVM does. `release.yml` ignores the tag (it takes `v*` only). It needs the same tag ruleset as `v*` (docs/54 section 10.2).
- (b) **Each version release.** Then version N's `toolchain.json` points at a zip attached to version N-1, or the sums wait one release.

## Licence

- **LLVM**: Apache License 2.0 with LLVM Exceptions (read, `llvm/LICENSE.TXT`). Apache 2.0 asks for a copy of the licence with the files, and any NOTICE file. LLVM has none: the top folder, `llvm/`, `clang/` and `lld/` hold `LICENSE.TXT` files and `llvm/CREDITS.TXT` only (read, the source tree at the tag).
- **The licence files might not be in the archive.** LLVM's install rules copy `LICENSE.TXT` only inside `include/llvm` (read, `llvm/CMakeLists.txt`); the installer shows the licence on a page. So the job takes `llvm/`, `clang/` and `lld/LICENSE.TXT` from the source at the tag, pinned by SHA-256. The summary lists any licence files the archive does hold.
- **libxml2.** LLVM's Windows build links libxml2 2.9.12 statically (read: `LLVM_ENABLE_LIBXML2=FORCE_ON` and `libxml2s.lib` in `build_llvm_release.bat`). lld-link uses it to merge manifests (inferred). Its licence is MIT-style and asks for its notice, so the zip carries libxml2's `Copyright` file, pinned too.
- **rpmalloc** (`LLVM_ENABLE_RPMALLOC=ON`): public domain, or MIT if you prefer (read, its README at the tag). Nothing to ship.
- **We change no file.** The zip is a subset of LLVM's files, and `README.txt` says where they came from.
- The game DLL that clang builds on the player's PC is never distributed (docs/52 section 11).

## Open questions

1. **Which release carries the zip**: its own `llvm-22.1.8-1` release (recommended), or the version releases. *Decided: its own release (10-22), tagged on master and still a draft.*
2. **LZMA or deflate**: LZMA by default, about 49 MB against about 69 MB. A dry run gives the real sizes. *LZMA, as made: 49,496,165 bytes, 196,460,103 unpacked (measured). Deflate was not run.*
3. **`__chkstk`.** A function with more than 4 KB of locals makes clang call `__chkstk`, which comes from compiler-rt or the C runtime. docs/57's list of outside symbols has no `__chkstk` (read), so today's lift may not need it. If the freestanding link (docs/52 section 7.8) reports it, the game part's support file can define it, or the zip can add `clang_rt.builtins-x86_64.lib` (190 KB, docs/52; package revision 2). Not checked.
4. **Same bytes as the installer?** Compare the JSON's sums for `clang.exe` and `lld-link.exe` with a developer's install, once. If they match, docs/57's builds and setup's builds use the same compiler bytes. *No (measured 10-22): both report 22.1.8, but the zip's `clang.exe` is 108,434,432 bytes against the installer's 104,745,472, and `lld-link.exe` 72,403,968 against 71,456,768. They are separate builds of the same version. So a game DLL built with the zip has to be checked on its own: a setup compile with the pinned zip, then a boot.*
5. **Signing.** LLVM's exes are unsigned (the run reported it, 10-22). Check whether Smart App Control or Defender blocks an unsigned `clang.exe` unpacked from a download. This is the same clean-PC test docs/54 section 10.1 asks for the game DLL.
6. **Linux**: the same job for LLVM's Linux archive (docs/87).

## Sources

- The LLVM 22.1.8 release: `https://github.com/llvm/llvm-project/releases/tag/llvmorg-22.1.8`, its text and asset list through `https://api.github.com/repos/llvm/llvm-project/releases/tags/llvmorg-22.1.8` (the `digest` fields), read 2026-10-22.
- The attestation for the archive's digest: `https://api.github.com/repos/llvm/llvm-project/attestations/sha256:d96c2cc1736f4eb7fa43cb9bbdf56d93551a9ae0a9aadb9c99c3c3b2b712a234`, read 2026-10-22.
- LLVM's release keys: `https://releases.llvm.org/release-keys.asc`, read 2026-10-22.
- LLVM's source at `llvmorg-22.1.8`: `.github/workflows/release-binaries.yml`, `llvm/utils/release/build_llvm_release.bat`, `llvm/CMakeLists.txt`, `llvm/LICENSE.TXT`, `clang/LICENSE.TXT`, `lld/LICENSE.TXT`, `llvm/lib/Support/rpmalloc/README.md`.
- libxml2 v2.9.12's `Copyright`: `https://gitlab.gnome.org/GNOME/libxml2/-/raw/v2.9.12/Copyright`.
- GitHub CLI manual, `gh attestation verify`: `https://cli.github.com/manual/gh_attestation_verify`. GitHub CLI source, `pkg/cmd/release/shared/fetch.go` (`gh release upload` finds a draft by its tag).
- GitHub docs, manually running a workflow: `https://docs.github.com/en/actions/how-tos/manage-workflow-runs/manually-run-a-workflow`.
- docs/45 (sizes), docs/52 (sections 6, 7.8 and 11), docs/54 (CI and releases), docs/57 (section 6.2), docs/81 (section 3).
