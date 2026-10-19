# Versioning, pinning and releases (design, 2026-10-18)

This is a design, plus branches for the parts that were small enough to prepare. The branches are open as draft PRs (below); none is merged. No tag exists and no release was made. Builds went through the build-slot lock. Nothing ran the game.

Three labels, as in docs/45 and docs/52:
- **(read)**: seen in code, a doc or a web page.
- **(measured)**: counted, built or compared on this PC.
- **(inferred)**: my conclusion, not checked.

Related docs. This one points to them and does not repeat them:
- docs/05: what may never be committed.
- docs/45: updates, integrity, signing options, the lift key.
- docs/52: player setup and the toolchain bundled for players (written at the same time).

## Answer first

**Version scheme.**
- SemVer, `MAJOR.MINOR.PATCH[-pre]`, in a one-line `VERSION` file at the repository root.
- A release is the tag `v<VERSION>` on master: `v0.1.0`, or `v0.2.0-rc.1` for a pre-release.
- Builds between releases name their commit: `0.1.0-dev.326+g495d5a7b3`, plus `.dirty` when tracked files are changed.
- Proposed first release: `0.1.0`.

**Fork tags.**
- `des-<upstream version>-<n>`. For example, `des-0.14.0-1` is the fork's first release on top of upstream v0.14.0.
- They can't clash with upstream's `v*` tags.
- They never start upstream's release workflow. That workflow publishes a release for any `v*` tag pushed to the fork (read, section 3).

**Pinning.**
- A `ps3recomp.pin` file at the port root names the fork's repository, a `des-*` tag (`none` between releases) and a commit.
- CI checks it against the fork on every pull request.
- CMake checks your local checkout against it: a warning by default, an error with `-DDES_REQUIRE_PIN=ON`.
- A release must pin a tag. Not a submodule (section 4).

**Same exe on every PC.**
- The tools can already do it. clang-cl and lld-link 22.1.8 make byte-identical objects and exes from two different folders with `/Brepro` and prefix maps (measured, section 5).
- The PPU lift is deterministic too (measured).
- What is missing: those flags in our CMake files, dependencies pinned by commit, and a pinned toolchain. For players, the toolchain is docs/52's bundled LLVM.
- Publish the expected hash per release once docs/52's game DLL exists. Until then the exe also depends on each PC's Visual Studio and Windows SDK (inferred).

**Version display.** Done on a branch. These show the port version, the ps3recomp tag or commit, and the SHA-256 of the game dump (measured, section 6):
- the exe's version resource;
- the start screen;
- Settings > About;
- a line near the top of the log;
- issue reports and crash reports.

**CI.** Nothing in CI needs the game. Upstream's workflows in the fork are not touched (section 7).
- Port:
  - a game-file check;
  - a Windows build of the wrapper UI;
  - script checks on Linux;
  - the pin check;
  - a release dry run.
- Fork:
  - the same game-file check;
  - a release workflow for `des-*` tags.

**Release.**
- Pushing `v0.1.0` drafts a GitHub release with three parts:
  - notes from the merged pull request titles;
  - a source zip of both repositories at matched versions, with no game files;
  - `SHA256SUMS`.
- The zip is byte-identical on every run (measured).
- The release stays a draft until you publish it (section 8).

**Game-file check.**
- It flags these by name, folder and content:
  - the lift and the EBOOT;
  - firmware modules;
  - gamedata;
  - shader caches and dumps;
  - frame captures and screenshots;
  - Ghidra output.
- Both trees and the port's history are clean (measured).
- The fork's history is not. Six PS3 firmware modules (`libsre.prx` and five audio modules) are in old commits of both upstream and the fork (section 9).

**Signing.**
- Not worth it yet: nothing prebuilt ships.
- Sign once docs/52's host exe ships. Try SignPath Foundation first (free for open source), then Azure Artifact Signing (section 10).

### Branches ready (draft PRs)

Every branch starts from master as it was at 495d5a7 (port) and f15a4f0 (fork). Each one merges cleanly into today's master (port 170301f, fork dedfdaf), and every pair of port branches merges cleanly (measured, `git merge-tree`). Opened as draft PRs on 2026-10-18 with the same commits.

| Repo | PR | Branch | Commit | What |
|---|---|---|---|---|
| ps3recomp | #58 | `tools/ci-game-files` | 56c046f | The game-file check, with 3 exceptions for the fork's own files |
| ps3recomp | #59 | `tools/des-release-tags` | d64df77 | `des-release.yml`: a draft release from a `des-*` tag |
| port | #64 | `tools/ci-game-files` | 5054895 | The game-file check |
| port | #65 | `tools/ci-port` | 22ee635 | `ci.yml`: UI build on Windows, script checks on Linux |
| port | #66 | `tools/ps3recomp-pin` | 54606b8 | `ps3recomp.pin`, its CMake check, `tools/release/pin_ps3recomp.py`, `pin.yml` |
| port | #62 | `feat/version-display` | dac1522 | `VERSION`, `port/cmake/DesVersion.cmake`, the version in the exe, UI, log and reports |
| port | #63 | `tools/release-workflow` | da3238c | `release.yml`, `release-check.yml`, `tools/release/changelog.py` and `package.py` |

Not prepared: the reproducible-build flags (section 5.4). They change everyone's build, so they wait for your decision.

### Decisions for you

1. **Version scheme.** SemVer, `VERSION` and `v` tags. First release `0.1.0` (proposed) or `0.1.0-alpha.1`.
2. **Fork tag prefix.** `des-` (proposed) or `fork-`.
3. **Pin file or submodule.** The pin file is proposed (section 4.1).
4. **Pin check on pull requests.**
   - Proposed: it fails. A port PR that moves the pin stays red until its fork PR merges; then re-run the check.
   - Alternative: warn on pull requests, fail only on master.
5. **Reproducible-build flags (section 5.4).**
   - Two PRs, one in the fork and one in the port.
   - Everyone rebuilds everything once.
   - Adopt them, and when?
6. **Actions.** Pin GitHub actions to commit SHAs (proposed, with item 5), or keep `@v4`-style tags as upstream does.
7. **Signing.** Wait until docs/52's host exe ships, then apply to SignPath Foundation (section 10).
8. **Firmware files in old commits** (section 9.3).
   - Tell upstream.
   - Or coordinate a history rewrite with them.
   - Or leave it.
9. **GitHub settings, after the merges.** Required checks (game-files, ci, pin), tag rulesets for `v*` and `des-*`, immutable releases (sections 7.3 and 10.2).
10. **First tags.** When to cut `des-0.14.0-1` and `v0.1.0`. Nothing is tagged until you say so.

## 1. Today

**The port.**
- Tags: only `override-bootstrap-v1`. There are no releases and no workflows; master has no `.github/` folder (measured).
- The version: `port/ui/CMakeLists.txt` runs `git rev-parse --short HEAD` at configure time. It passes the result as `DES_VERSION` to three files only: `prelaunch.cpp`, `settings_window.cpp` and `report.cpp` (read).
  - It goes stale after a commit until CMake configures again (inferred).

**How the port finds ps3recomp.**
- `PS3RECOMP_DIR` defaults to `<repo>/ps3recomp` (read, `port/CMakeLists.txt` line 10).
- It links the prebuilt `${PS3RECOMP_DIR}/build/ps3recomp_runtime.lib` and compiles a few runtime sources directly (read).
- `/ps3recomp/` is in `.gitignore` (read).
- README step 1 says to clone the fork into `ps3recomp/`. It names no branch, tag or commit (read).
- Nothing records which fork commit a build used.

**The fork.**
- On GitHub it has no tags (measured, `git ls-remote --tags`).
- The local clone has upstream's tags `v0.9.0` to `v0.15.0` plus `override-bootstrap-v1` (measured).
- Fork master describes as `v0.14.0-557-gdedfdaf`, so upstream v0.15.0 is not merged (measured).

**The fork's workflows.** All come from upstream (read):
- `windows.yml`, `linux.yml`, `linux-vulkan.yml` and `macos.yml` run on master pushes and pull requests.
- `release.yml` runs on `tags: ['v*']`. It runs `gh release create "$TAG" --title "$TAG" --generate-notes` when no release exists yet, then uploads SDK zips.
- So any `v*` tag pushed to the fork would publish a release there (inferred from the file).

**What the current exe embeds.** Measured on the main tree's `des_boot.exe` (298,684,928 bytes):
- A real link timestamp.
- 114 absolute paths: RmlUi's `__FILE__` strings under `port/build/_deps`.
- No debug directory: it is linked without `/DEBUG`, and the PDB beside it is stale.
- No `__DATE__` or `__TIME__`: neither repository uses them.
- No version resource.
- Imports of `MSVCP140`, `VCRUNTIME140` (`/MD`) and `MFPlat`.

**Dependencies.**
- FreeType and RmlUi are fetched at configure time by tag: `GIT_TAG VER-2-14-3` and `GIT_TAG 6.3` (read).
- The fork's `requirements.txt` uses `>=` versions (read). The lift tools (`ppu_loader.py`, `ppu_lifter.py`, `spu_lifter.py`, `gen_hle_nids.py`) import none of those packages (measured, grep of imports). Only `pkg_extract.py`, `extract_jobmods.py` and the disassembly audits use them.

**Toolchain on this PC.** LLVM 22.1.8 (measured), with Visual Studio 2022 Build Tools (read, build scripts).

## 2. Version scheme

### 2.1 The rule

- **`VERSION`.** One line: `x.y.z` or `x.y.z-pre`. It holds the release being worked toward, or the one just made.
- **A release** is an annotated tag `v<VERSION>` on a master commit whose `VERSION` says exactly that. The release workflow refuses anything else (section 8).
- **Pre-releases** use `-alpha.N`, `-beta.N` or `-rc.N`.
  - A tag with a `-` becomes a GitHub pre-release.
  - GitHub's "latest" skips pre-releases (docs/45 section 5.1).
- **When to bump** (proposal, inferred):
  - PATCH: fixes only.
  - MINOR: new features or settings.
  - MAJOR stays 0 until the game plays start to end and docs/52's player setup works. That is 1.0.0.

### 2.2 What each build shows

From `port/cmake/DesVersion.cmake` on `feat/version-display` (measured for the dev case; the others read from the code):

| Build | Shows |
|---|---|
| The commit tagged `v0.1.0` | `0.1.0` |
| Before the tag exists (`VERSION` is 0.1.0) | `0.1.0-dev.<n>+g<commit>`, where n counts commits since the last `v` tag, or since the start |
| After the tag, before `VERSION` moves on | `0.1.0+<commits since v0.1.0>.g<commit>` |
| `VERSION` is a pre-release (0.2.0-rc.1), not tagged yet | `0.2.0-rc.1.dev.<n>+g<commit>` |
| Changes to tracked files | adds `.dirty` (or `+dirty`) |
| Shallow clone (CI) | no count: `0.1.0-dev+g<commit>` |
| No git: a release zip | the version in `release-info.txt`, written by the packager |
| No git and no `release-info.txt` | `0.1.0-dev+unknown` |

Today's master would show `0.1.0-dev.326+g495d5a7b3` (measured): 326 commits and no `v` tag yet.

Why this shape (inferred):
- **SemVer order.** `0.1.0-dev.N` sorts below `0.1.0`, so docs/45's update check would offer 0.1.0 to a dev build. SemVer ignores everything after `+` when it compares.
- **One edge case.** `0.2.0-rc.1.dev.5` sorts above `0.2.0-rc.1`. So the update check should compare only release builds and skip any version that contains `dev` or `+g`.
- **Always unique.** The commit is always in the string. Two builds show the same string only when they come from the same commit.

### 2.3 The exe's version resource

- FILEVERSION and PRODUCTVERSION are `x,y,z,<commits>`, with 0 as the last part for a release.
  - Each part is a 16-bit number (read, Microsoft's VERSIONINFO reference), so the count stops at 65535.
- FILEFLAGS is `VS_FF_PRERELEASE` for anything but a release.
- The strings: FileDescription, ProductName, FileVersion, ProductVersion, InternalName, OriginalFilename, and Comments (ps3recomp and the dump hash).
- Explorer shows them under Properties > Details. PowerShell reads `FileVersionRaw 0.1.0.326` and `IsPreRelease True` (measured).
- There is no CompanyName or LegalCopyright.
  - Microsoft lists CompanyName as required (read). Windows reads the resource without it (measured).
  - Add both with a project name, not a person's, if you want them.

### 2.4 Alternatives considered (inferred)

- **CalVer** (`2026.10.0`) says when, not what changed. GitHub's "latest" and FFCCEOT's `is_newer` (docs/45 section 5.1) work with either scheme. No gain.
- **A build number only** means nothing to players.
- **ps3recomp's numbers.** Port releases don't line up with recompiler releases.

## 3. Fork tag scheme

### 3.1 The rule

- **Form.** `des-<upstream base>-<n>`.
  - The base is the newest upstream release merged into fork master. `git describe --tags --abbrev=0 --match "v[0-9]*" fork/master` gives `v0.14.0` today (measured).
  - n counts fork releases on that base, from 1.
- **Examples.** `des-0.14.0-1`, then `des-0.14.0-2`. After merging upstream v0.15.0: `des-0.15.0-1`.
- **Annotated**, on fork master.
- **Pushed one at a time**: `git -C ps3recomp push fork des-0.14.0-1`.
  - Never `git push --tags` or `--follow-tags`. The local clone holds nine tags, eight of them upstream's `v*` releases (measured). Pushing them to the fork would run upstream's `release.yml` there, and it would publish releases (inferred from the workflow).
  - Never push to `origin`; that is upstream.
- **What the port shows** (from `git describe`):
  - `des-0.14.0-1` at the tag;
  - `des-0.14.0-1-5-g<12 hex>` five commits after it;
  - the bare commit when no `des-*` tag is reachable (measured: `56c046fc9067` for a branch).

### 3.2 Why this shape

- **No clash with upstream.** Upstream's tags are `v<x.y.z>` (read). `des-` never matches `v*`.
- **Not `v0.14.0-des.1`.** Upstream's `release.yml` triggers on `v*` (read), so that tag would start it in the fork and publish an SDK release. It would also look like an upstream pre-release.
- **It says which upstream** the fork release is built on.
- **Alternatives.**
  - `fork-0.14.0-1`: works the same, but says less.
  - `des/0.14.0/1`: needs `**` in workflow filters and looks like a path.

### 3.3 The fork's release workflow

`.github/workflows/des-release.yml`, on `tools/des-release-tags`:
- **Trigger.** Only tags matching `des-[0-9]+.[0-9]+.[0-9]+-[0-9]+`.
- **Job 1, Windows.**
  - Builds the runtime with clang-cl and Ninja (Release).
  - Runs `tools/test_*.py` and `tools/check_ppu_scaffold.py`.
- **Job 2, the draft.**
  - The tag must be on master. A lightweight tag gets a warning.
  - The notes list the fork's own pull requests since the previous `des-*` tag (oldest first) and the "Merge upstream" commits.
  - Then it runs `gh release create --draft --verify-tag`. This is the only job with `contents: write`.
- **Measured** in a throwaway clone with local tags:
  - `des-0.14.0-1` lists all 49 fork pull requests and the 2 upstream merges.
  - A later `des-0.14.0-2` lists only the pull requests after the first tag.
- **No binaries.** The port builds the runtime from source with its own flags (read, README). Upstream's SDK zip is not used.

## 4. Pinning

### 4.1 Options

| Option | For | Against |
|---|---|---|
| Submodule | Git records the commit. `git clone --recursive` fetches it. | It pins a commit, not a tag. Your `ps3recomp/` is a separate clone with its own work, and lanes build against separate worktrees. A submodule would change every checkout (inferred). |
| Pin file and checks (chosen) | One small text file. Works with the nested clone, worktrees and `PS3RECOMP_DIR`. A release can require a tag. | Nothing fetches the fork for you; a check tells you what to do. |
| FetchContent of the fork | CMake fetches it. | Fights local fork work. It is a download during the build, and docs/52 says setup fetches nothing. |
| Copy the fork's source into the port | One repository. | Huge diffs. Upstream merges get harder. |

### 4.2 The file

```
# (a header comment that explains the fields)
repo=https://github.com/<owner>/ps3recomp
tag=none
commit=f15a4f0b6dcc1a8f1732392bf926ae16d311db49
```

- **At a release:** `tag=des-x.y.z-n`, and `commit=` the tag's commit.
- **Between releases:** `tag=none`, and `commit=` the oldest fork commit this port revision works with.
- **When a port PR needs a newer fork change,** it moves `commit=` in the same PR, after the fork PR merges.
  - PRs are merged here with merge commits, so the fork PR's last commit is on fork master after the merge. Either it or the merge commit works (inferred).
  - A squash merge would not keep it there.
- **Editing it.** `python tools/release/pin_ps3recomp.py set --commit <rev> --checkout ps3recomp`, or `--tag des-...`. The script keeps the header and the line endings (measured).

### 4.3 Checks

| Where | What it checks | When it fails |
|---|---|---|
| `pin.yml`, every PR and master push | `pin_ps3recomp.py check --remote`: the file parses; the tag, if any, exists on the fork and points at the commit; the commit is on fork master. It uses a treeless clone (3.6 s, measured). | The commit is not on fork master yet. A port PR that moves the pin is red until its fork PR merges. |
| `release.yml` | `check --remote --release`: the same, and the tag must be a `des-*` tag. | A release pins an untagged commit. |
| CMake, default | The checkout contains the commit (`git merge-base --is-ancestor`). | Warning: "does not contain the commit this port needs ... Update it to the fork's master." |
| CMake, `-DDES_REQUIRE_PIN=ON` | HEAD is exactly the commit, with no changes to tracked files. | Error. Meant for release builds and CI. |
| CMake, `PS3RECOMP_DIR` without `.git` | Compares with `ps3recomp/ps3recomp.pin`, the copy the release zip carries. | Warning, or an error with `DES_REQUIRE_PIN`. |

Measured:
- Every CMake case: dev mode, require mode, no git with and without the copy, a broken file, a missing file.
- A side commit merged through a pull request passes the remote check.
- A made-up tag or commit fails with a clear message.
- The git calls use `--no-optional-locks`, so a check never rewrites the index of a checkout it only reads.

Your nested `ps3recomp/` is at 8b77615, 14 commits behind fork master. It does not contain the pinned f15a4f0 (measured). Once the pin is merged, configure will warn until you update it.

The pin never changes files. `pin_ps3recomp.py local` reports; nothing checks out a commit for you. Lanes and your checkout keep their own state.

Follow-up, not on a branch: README step 1 could add "then check out the commit in `ps3recomp.pin`". `README.md` has uncommitted changes in your checkout, so I left it alone.

## 5. Same exe on every PC

### 5.1 What it needs

For the same release and the same dump to give the same bytes on every PC:
1. **The same lift.** The PPU lift is deterministic (measured, 5.3). The SPU lift was not checked.
2. **The same compiler and linker,** same version: the toolchain bundled for players (docs/52 section 6.1).
3. **The same flags,** set by the build files, not by whoever builds.
4. **No timestamps:** `/Brepro` for clang-cl objects and for lld-link.
5. **No machine paths:** prefix maps, relative object paths, no absolute PDB path. A player's install folder is different on every PC.
6. **The same inputs in the same order.** CMake's glob of the lift's chunks comes back sorted (inferred).
7. **Pinned dependencies:**
   - FreeType and RmlUi by commit;
   - the fork by the pin;
   - GitHub actions by SHA.

### 5.2 Measured: what makes two builds differ

Setup:
- Two small sources, one C and one C++, both using `__FILE__`.
- Each compiled in two folders with different names and depths.
- LLVM 22.1.8 clang-cl and lld-link. SHA-256 compared.

Objects:

| Compile flags | Objects | Why they differ |
|---|---|---|
| `/O2` | differ | timestamp and paths |
| `/O2 /Brepro` | differ | `__FILE__` paths |
| `/O2 /Brepro -ffile-prefix-map=<root>=.` | **same** | |
| the same plus `/Zi`, with or without `-fdebug-compilation-dir=.` and `-gno-codeview-command-line` | differ | the CodeView S_OBJNAME record holds the absolute `/Fo` path |
| the same with all three, and a relative `/Fo` (the way CMake's Ninja generator passes it) | **same** | |
| the same with all three, an absolute `/Fo` and `-Xclang -object-file-name=<name>` | **same** | |

Links, from identical objects:

| Link flags | Exe | PDB |
|---|---|---|
| none | differs (timestamp) | |
| `/Brepro` | **same** | |
| `/DEBUG /Brepro` | differs | not compared |
| `/DEBUG /Brepro /pdbaltpath:%_PDB%` | differs | differs |
| `/DEBUG /Brepro /pdbaltpath:%_PDB% /pdbsourcepath:<fixed>` | **same** | **same** |

Also measured:
- `llvm-lib` archives are identical. So is `llvm-rc` output (a VERSIONINFO plus an icon made by `scripts/make_icon.py`).
- **A small CMake and Ninja project** has a generated source in the build folder and is linked through `cmake -E vs_link_exe`, like the port.
  - With `/Brepro` on compile and link, and prefix maps for the source and build folders, the exes are identical.
  - Without them, the exes differ and embed both absolute paths.
- **Overlapping prefix maps:** when two maps match a path, the longer one wins, in either order.
- **Dates:** `SOURCE_DATE_EPOCH` sets `__DATE__`. `-Werror=date-time` turns any use of it into an error.
- **Path length:** deep build folders break the port's build. CMake's object path limit is 250 characters, and a lane folder reached 262 (measured). Keep build folders short.

### 5.3 Measured: the lift

This answers docs/45's run request R5. Section 5.2 answers R6 for small sources; chunk 012 itself was not compiled twice.
- Three PPU lifts with `PYTHONHASHSEED` 1, 2 and 3, and `-j` 8, 8 and 4: all 15 files identical.
- They are also identical to the shared lift the lanes use.
- `gen_hle_nids.py` run twice: identical, and equal to that lift's table.
- The SPU lift (`build_spu.py`) was not checked.

### 5.4 Plan (proposed, not implemented)

Two PRs: P6a in the fork (the runtime library) and P6b in the port.
1. **`/Brepro`** on every compile and link.
2. **Prefix maps** for every root a build reads:
   - the source root;
   - the build folder;
   - `PS3RECOMP_DIR`;
   - `RECOMP_DIR` (the lift);
   - the FetchContent folders.
   The longer map wins, so nested roots are fine (measured).
3. **`-Werror=date-time`.**
4. **No debug info in player builds.** Developers keep what they use. With `/DEBUG`, add `/pdbaltpath:%_PDB%` and `/pdbsourcepath:`.
5. **FetchContent by commit** (`GIT_TAG <40 hex>`), or `URL` with `URL_HASH`.
6. **Actions pinned by commit SHA.** GitHub recommends it, because "a tag can be moved or deleted" (read).
7. **The toolchain.**
   - Players: docs/52's bundled LLVM 22.1.8 and embeddable Python (sections 6.1 and 6.2).
   - CI: compare `clang-cl --version` with the pinned version and fail on a mismatch. Runner images change their tools over time (inferred).
8. **Expected hashes.** Publish the SHA-256 of the game DLL per release, per supported dump and per flag set. docs/45 sections 4.5 and 5.2 cover this (`expected_dll` in the manifest). The setup screen compares it (docs/52).

Outline for the port (P6a is the same in the fork's `CMakeLists.txt`):

```cmake
# Byte-identical builds (docs/54 section 5): no timestamps, no machine paths.
option(DES_REPRO "byte-identical builds" ON)
if(DES_REPRO AND MSVC AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  # (plus FETCHCONTENT_BASE_DIR and any FETCHCONTENT_SOURCE_DIR_<NAME> in use)
  foreach(root IN ITEMS "${DES_SOURCE_ROOT}=." "${CMAKE_BINARY_DIR}=build"
                        "${PS3RECOMP_DIR}=ps3recomp" "${RECOMP_DIR}=lift")
    add_compile_options("/clang:-ffile-prefix-map=${root}")
  endforeach()
  add_compile_options(/Brepro /clang:-Werror=date-time)
  add_link_options(/Brepro)
endif()
# before add_subdirectory(ui), so RmlUi and FreeType get the same options
```

Cost:
- Every object changes once, so everyone rebuilds everything.
- This lane's full `des_boot` build took about 21 minutes with 3 jobs while another build ran (measured).

Why it is not a PR yet:
- It changes everyone's build flags, so it is your call.
- **Limit (inferred).** Today's all-in-one exe also links code from the installed Visual Studio and Windows SDK: import libraries, CRT startup code and STL headers. So even with these flags, two PCs make the same exe only if they have the same Visual Studio and SDK versions. docs/52's route B takes those out of the player's build: the host is built once in CI, and the game DLL needs only the bundled clang and lld.

### 5.5 What differs per PC and does not matter

Shader caches (per GPU and driver), settings, logs and saves. None of them is part of the exe.

## 6. Version display (`feat/version-display`)

### 6.1 Where it shows

| Place | What |
|---|---|
| Start screen | "Version 0.1.0-dev...", as before, now from `VERSION` |
| Settings > About | **Version**. **ps3recomp** (new). **Game dump** (new): the first 12 hex digits; the help pane shows the full hash. |
| Log, near the top (just before `[ui] settings:`) | `[des] DemonsSoulsRecomp <version>, ps3recomp <tag or commit>, game dump <12 hex>` |
| Issue report, `system.txt` | `DemonsSoulsRecomp <version>`, `ps3recomp: <x>`, `Game dump (EBOOT.ELF SHA-256): <hash>` |
| Crash report | The header names the version. A new line: `ps3recomp <x>, game dump <hash>`. |
| Explorer, Properties > Details | File version `0.1.0.<commits>`, flagged pre-release between releases. Product version: the full string. Comments: ps3recomp and the dump hash. |

### 6.2 How it works (read, the branch)

- **Two generated files.** `port/cmake/DesVersion.cmake` writes `des_version.h` and `des_version.rc` in the build folder.
- **Every build.** A custom target runs the module on every build (`cmake -P`).
  - It rewrites the files only when their text changes. Ninja then sees no change (BYPRODUCTS), so an unchanged version recompiles nothing.
  - A commit shows without configuring again (measured, 6.3).
- **The dump hash** is the SHA-256 of `DES_DUMP_ELF` (default `work/game/EBOOT.ELF`), taken at configure time. The file is a configure dependency, so a new dump configures again.
- **ps3recomp.** `git describe --tags --match "des-*" --always --abbrev=12 --dirty=-dirty` in `PS3RECOMP_DIR`.
  - Without git: the copy of `ps3recomp.pin` the release zip carries.
  - Otherwise "unknown".
- **Read-only git.** It reads git with `--no-optional-locks`, so a build never rewrites the index of a checkout.
- **No time and no paths** in the output. The same commit gives the same header, from a git checkout or from the release zip (inferred from the code).
- **What it replaces:** the configure-time `git rev-parse --short` and the per-file define. The UI built alone (as in CI) includes the same module.

### 6.3 Measured

- The full `des_boot` build of the branch passed: about 21 minutes with 3 jobs; the exe is 299,317,760 bytes.
  - It was made before the commit, from the changed tree, so it showed `0.1.0-dev.326+g495d5a7b3.dirty`.
- The generated header and resource hold what section 2.2 says.
- PowerShell's `VersionInfo` for that build reads:
  - FileVersionRaw 0.1.0.326;
  - IsPreRelease True;
  - ProductName DemonsSoulsRecomp;
  - Comments with ps3recomp and the dump hash.
- The log line format and the report lines are in the exe.
- **After the commit,** a build without configuring again ran 9 steps in 16 seconds:
  - the version step;
  - the 5 UI sources that include the header, and `des_ui.lib`;
  - the resource and the link.
  The exe then reads FileVersionRaw 0.1.0.327 and FileVersion `0.1.0-dev.327+gdac152274`, without `.dirty`.
- The UI built alone, as CI builds it, passed.
- The game was not run (run request R1).

### 6.4 The dump hash

- It is the SHA-256 of the decrypted `EBOOT.ELF` the lift was made from.
- It should be the same for every copy of the same game release, and differ between regions and versions (inferred). A table of known hashes can come later (docs/45 step 7).
- It identifies a game release, not a player (inferred).

## 7. CI

### 7.1 Port

| Workflow | Branch | Runs on | Does |
|---|---|---|---|
| `game-files.yml` | `tools/ci-game-files` | PRs, master pushes | Self-test. Every tracked file. Every commit of the PR or push. |
| `ci.yml` | `tools/ci-port` | PRs, master pushes | **Windows:** builds the wrapper UI (des_ui, RmlUi, FreeType) with clang-cl and Ninja through `.github/ci/des_ui`. **Linux:** compiles every tracked `.py`, runs `bash -n` on every `.sh`, and runs `make_icon.py` on a made-up picture. |
| `pin.yml` | `tools/ps3recomp-pin` | PRs, master pushes | Pin self-test. The pin against the fork. |
| `release-check.yml` | `tools/release-workflow` | PRs that touch `tools/release/`, the release workflows, `ps3recomp.pin` or `VERSION` | The notes. The zip made twice and compared. The game-file check of the zip's contents. |
| `release.yml` | `tools/release-workflow` | `v` tags only | Section 8. |

What CI can't do:
- build `des_boot`, because it needs the lift, which is game-derived;
- run the shader precompile;
- run the game.

After the DLL split (docs/45 step 4, docs/52), CI can build the whole host.

Measured on this PC:
- The UI build alone: 285 steps; `des_ui.lib` is 18.8 MB.
- All 16 tracked `.py` files compile.
- The icon step passes.
- The game-file self-test passes its 18 cases.

None of these workflows has run on GitHub yet.

### 7.2 Fork

| Workflow | Branch | Note |
|---|---|---|
| upstream's `windows.yml`, `linux.yml`, `linux-vulkan.yml`, `macos.yml` | (master) | Not touched. `windows.yml` already builds the runtime and runs the lifter test suites, the PPU scaffold check and the self-contained SPU and network tests on every pull request (read). Lane f3-ci fixed Linux and macOS (fork PR #52, merged). |
| upstream's `release.yml` (`v*` tags) | (master) | Not touched. Never push `v*` tags to the fork. |
| `game-files.yml` | `tools/ci-game-files` | The same script and workflow, with 3 exceptions (section 9.2). |
| `des-release.yml` | `tools/des-release-tags` | Section 3.3. |

### 7.3 Settings to change by hand, after the merges

- **Required checks on master:** game-files, both ci jobs, and pin.
  - Not release-check. It has path filters, and a required check that a path filter skips stays "Pending" and blocks the merge (read, GitHub's workflow syntax docs).
- **Tag rulesets** (section 10.2).

## 8. Release workflow (`tools/release-workflow`)

### 8.1 Steps

**Trigger:** a pushed tag matching `v[0-9]+.[0-9]+.[0-9]+` or `v[0-9]+.[0-9]+.[0-9]+-*`. In GitHub's filter syntax, `+` means one or more of the preceding character (read).
- These trigger it: `v0.1.0`, `v0.1.0-rc.1`, `v10.20.30` (inferred from the syntax).
- These don't: `des-*`, `v0.1`, `v0.1.0.1`, `override-bootstrap-v1` (inferred).

Steps:
1. **`package.py check-tag`.**
   - The tag exists.
   - `VERSION` at the tag equals it.
   - The tag is on master.
   - The pin has a `des-*` tag.
   - A lightweight tag gets a note.
2. **The pin against the fork**, in release mode.
3. **The notes** (`changelog.py`).
4. **The zip** (`package.py build`).
5. **The game-file check** of the staged folder.
6. **`gh release create <tag> --draft --verify-tag`**, plus `--prerelease` for a tag with a `-`. It uploads the zip and `SHA256SUMS`.
   - `--verify-tag` aborts if the tag is not on GitHub, so gh never makes a tag itself (read, gh manual).

Nothing is published. You read the draft and publish it. Only this workflow has `contents: write`; every other workflow is read-only.

### 8.2 The zip

- **Name.** `DemonsSoulsRecomp-<version>-source.zip`, next to `SHA256SUMS`.
- **Holds:**
  - the port at the tag (`git archive`), without `.github/` and `.claude/`;
  - ps3recomp at the pinned commit, in `ps3recomp/`, without `.github/`, `scratch/`, `packaging/` and the SPU test outputs;
  - `ps3recomp/ps3recomp.pin`, the copy the CMake check and the version use;
  - `release-info.txt` with version, commit, ps3recomp_tag and ps3recomp_commit.
- **Does not hold:** anything game-derived, any build output, any binary. Both projects are MIT-licensed (read, their LICENSE files).
- **Why a separate zip.** GitHub's automatic "Source code" archives don't include ps3recomp, because it is not a submodule (inferred). This zip has both at matched versions. It builds without git: the version comes from `release-info.txt`, the pin from the copy.
- **Deterministic:**
  - sorted entries;
  - fixed times (the tag commit's time);
  - fixed modes;
  - deflate level 9;
  - LF line endings (`git archive` with autocrlf off).
  CI pins Python 3.12, because the deflate output could change with zlib's version (inferred).
- **Measured** at a test revision:
  - 839 files, 5.2 MB.
  - The same SHA-256 on repeated runs, whether the fork came from GitHub or from a local checkout.
  - The same across Python 3.11 and 3.13 (an earlier variant of the script).
  - The game-file check of the staged folder is clean.
- **Later,** docs/45 section 5.2's parts join the same draft release: the host zip, the trimmed LLVM, the embeddable Python, `update.json` and its signature.

### 8.3 The notes

- **Where they come from.** Merged pull requests on master's first-parent line since the previous `v` tag, read from `Merge pull request #N from owner/branch`. The title comes from the merge commit's body.
- **Groups**, by branch prefix:
  - `feat/`, `feature/` and `ui/`: New and changed;
  - `fix/`, `bugfix/` and `hotfix/`: Fixes;
  - `perf/`: Performance;
  - `docs/`: Documentation;
  - `tools/`, `build/`, `ci/`, `test/`, `release/` and the like: Tools, build and CI;
  - anything else: Other.
- **Direct commits** on master are listed when there are 15 or fewer, otherwise counted.
- **The fork.** Pull requests merged between the previous pin and the new one, linked as `<owner>/ps3recomp#N`.
- **No GitHub API**, so it runs the same on your PC (inferred). It needs merge commits, which is how PRs are merged here.
- **Measured** on today's master: 45 pull requests, plus 180 direct commits counted (from before PRs were used).

### 8.4 First release checklist (for you; nothing done)

1. Merge the PRs, in the order in section 12.
2. **Fork tag.**
   - Run `git -C ps3recomp fetch fork`.
   - Run `git -C ps3recomp tag -a des-0.14.0-1 -m "ps3recomp for DemonsSoulsRecomp 0.1.0" fork/master`.
   - Run `git -C ps3recomp push fork des-0.14.0-1`.
   - Read the fork's draft release and publish it.
3. **Pin PR.** In a port PR, run `python tools/release/pin_ps3recomp.py set --tag des-0.14.0-1 --checkout ps3recomp`, and confirm that `VERSION` is 0.1.0. Merge it.
4. **Port tag.**
   - Run `git fetch origin`.
   - Run `git tag -a v0.1.0 -m "DemonsSoulsRecomp 0.1.0" origin/master`.
   - Run `git push origin v0.1.0`.
5. Read the draft release and publish it.
6. In the next PR, move `VERSION` to 0.1.1 or 0.2.0.

## 9. Game-file check

### 9.1 What it flags

- **Folders** anywhere in a path:
  - `work`, `gamedata`, `savedata`, `shots`;
  - `dev_hdd0`, `dev_hdd1`, `dev_flash`;
  - `PS3_GAME`, `USRDIR`;
  - `fw_spu`, `work_spu`, `shader_cache`.
- **Names:**
  - `EBOOT.BIN`, `EBOOT.ELF`, `PARAM.SFO`, `PARAM.PFD`, `ICON0.PNG`;
  - the lift (`ppu_recomp_*`, `spu_recomp.*`) and the loader's JSON;
  - libsre files;
  - `port/gen` outputs;
  - `des_boot.map`;
  - Ghidra text;
  - shader dumps (`vp_pick`, `fp_pick`, `duck_fp`).
- **Extensions:**
  - PS3 formats (`elf`, `self`, `sprx`, `prx`, `pup`, `pkg`, `rap`, `edat`, `sdat`, `sfo`);
  - game formats (`flver`, `tpf`, `hkx`, `dcx`, `bnd`, `bdt`, `msb`, `fsb`, `at3`, `pam`, `gtf` and more);
  - captures and images;
  - compiled shaders;
  - build outputs;
  - archives and disc images.
- **Contents:**
  - ELF, SCE and PE headers;
  - the lifters' "Auto-generated" header;
  - lift density (`ctx->gpr` counts);
  - Ghidra names;
  - files over 2 MB.
- **Every commit** of a PR or push, not only the last tree. A file added and deleted again inside a PR still lands in the history.
- **Exceptions** live in `.github/game-files-allow.txt`: one glob per line, each with the reason.
- **A self-test** with 18 made-up samples runs first, so a green run can't mean "checked nothing".

### 9.2 Results (measured)

- **Port:** the tree and the full history are clean, with no exceptions needed. Today's master, after PR #49, is clean too.
- **Fork tree:** 24 findings, all the fork's own files, covered by 3 exceptions:
  - SPU test outputs, lifted from programs the tests generate;
  - the lifter's conformance driver (synthetic cases);
  - the macOS icon.
  Today's fork master is clean.
- **Positive controls:** made-up files of each kind are flagged.

### 9.3 The fork's history (decision for you)

- **What was committed.** Two commits add the same 26 files under `fw_spu/` (measured; the blobs are identical):
  - the fork's 76f80b4 (2026-06-25);
  - upstream's 02b599d (same date and title).
  The files are six PS3 firmware modules plus lifted firmware SPU code. The modules: `libsre.prx` (239,344 bytes), `libadec`, `libat3dec`, `libatrac3multi`, `libatrac3plus` and `libatxdec`.
- **More files.** Upstream's 7121c5a (2026-07-18) adds more lifted firmware files.
- **Untracked since.** Upstream stopped tracking `fw_spu/` in 0952e1c (#210). The fork got that with the upstream v0.14.0 merge (724fec1).
- **Still in history.** Today's trees are clean, but the old commits still hold the files. They are in fork master's history, in 48 fork branches and in upstream's master (measured).
- **The check passes** because it looks only at new commits. Anyone can still fetch those files by commit from either repository (inferred).
- **Removing them** means rewriting upstream's commits too (inferred):
  - The fork merges upstream. Cleaning only the fork would split it from upstream, and the next upstream merge would bring the files back.
  - So it is upstream's call first.
  - A rewrite also needs force-pushes of every branch, and GitHub support to drop cached pull request refs.
- **Options:**
  - (a) tell upstream and leave it;
  - (b) a coordinated rewrite with upstream;
  - (c) do nothing.

## 10. Signing

### 10.1 Code signing

- **Now: no.** The release has no exe or DLL.
  - A zip can't carry an Authenticode signature (inferred).
  - `SHA256SUMS` and GitHub's asset digests cover integrity (docs/45 section 5.2).
- **When docs/52's host exe ships: yes.** Without a signature, every download shows "Windows protected your PC" until the player clicks through (docs/45 section 5.3).
- **Order to try** (all from docs/45 section 5.3):
  1. SignPath Foundation.
     - Free for OSI-licensed projects, and MIT qualifies.
     - The signer shown is "SignPath Foundation".
     - It needs CI builds and their review. Whether they accept a game port is unknown.
  2. Azure Artifact Signing: about $9.99 a month, for individuals only in the USA and Canada.
  3. An OV certificate: $150 to $300 a year, with the key on hardware.
- **EV is not worth it:** it no longer skips SmartScreen's reputation check.
- **What can't be signed:** the game DLL each PC builds for itself. It is unknown whether Smart App Control or WDAC blocks an unsigned DLL built on the same PC (inferred risk). docs/45 step 14's clean-PC tests should check it.

### 10.2 Cheaper protection, available now

- **Tag rulesets.** GitHub rulesets can restrict who creates, updates or deletes matching tags (read). Limit `v*` in the port and `des-*` in the fork to you.
- **Immutable releases** lock a release's tag and files once it is published (docs/45 section 5.2).
- **Signed tags** (SSH signing) are optional. GitHub shows them as verified (inferred).
- **Later:** a minisign signature on `update.json` (docs/45 section 5.2).

## 11. Workflow outlines

The full files are on the branches. Outlines:

```yaml
# port and fork: .github/workflows/game-files.yml
on: { push: { branches: [master] }, pull_request: {}, workflow_dispatch: {} }
permissions: { contents: read }
jobs:
  check:                                 # ubuntu-latest
    - checkout (fetch-depth: 0)
    - check_game_files.py --self-test
    - check_game_files.py --tree HEAD
    - if PR:   check_game_files.py --range "$BASE_SHA..$HEAD_SHA"
    - if push: check_game_files.py --range "$BEFORE..$GITHUB_SHA"   # tree only if BEFORE is gone

# port: .github/workflows/ci.yml
on: { push: { branches: [master] }, pull_request: {}, workflow_dispatch: {} }
permissions: { contents: read }
jobs:
  ui:                                    # windows-latest
    - checkout; ilammy/msvc-dev-cmd (x64); choco install ninja
    - cmake -S .github/ci/des_ui -B build/ui -G Ninja (clang-cl, no build type)
    - cmake --build build/ui; test -s build/ui/des_ui/des_ui.lib
  scripts:                               # ubuntu-latest, Python 3.12
    - py_compile every tracked .py; bash -n every tracked .sh
    - make_icon.py on a made-up 320x176 PNG; expect sizes 256, 64, 48, 32, 24, 16

# port: .github/workflows/pin.yml
on: { push: { branches: [master] }, pull_request: {}, workflow_dispatch: {} }
jobs:
  pin:                                   # ubuntu-latest, Python 3.12
    - pin_ps3recomp.py self-test
    - pin_ps3recomp.py check --remote

# port: .github/workflows/release.yml
on: { push: { tags: ['v[0-9]+.[0-9]+.[0-9]+', 'v[0-9]+.[0-9]+.[0-9]+-*'] } }
permissions: { contents: write }
concurrency: release-<tag>
jobs:
  draft:                                 # ubuntu-latest, Python 3.12, fetch-depth 0
    - package.py check-tag "$TAG"
    - pin_ps3recomp.py check --remote --release
    - changelog.py --to "$TAG" --out dist/notes.md
    - package.py build --rev "$TAG" --out dist
    - check_game_files.py --dir dist/stage
    - gh release create "$TAG" --draft --verify-tag [--prerelease] zip SHA256SUMS

# port: .github/workflows/release-check.yml
on: { pull_request: { paths: [tools/release/**, .github/workflows/release*.yml, ps3recomp.pin, VERSION] },
      workflow_dispatch: {} }
jobs:
  dry-run:
    - changelog.py; package.py build --dry-run twice; cmp SHA256SUMS; check_game_files.py --dir

# fork: .github/workflows/des-release.yml
on: { push: { tags: ['des-[0-9]+.[0-9]+.[0-9]+-[0-9]+'] } }
permissions: { contents: read }
jobs:
  test:                                  # windows-latest
    - cmake configure (clang-cl, Ninja, Release); cmake --build build
    - tools/test_*.py (CI=1); tools/check_ppu_scaffold.py
  draft:                                 # needs test; contents: write
    - tag on origin/master (warn if lightweight)
    - notes: owner's PRs since the previous des-* tag, plus "Merge upstream" commits
    - gh release create "$TAG" --draft --verify-tag --notes-file notes.md
```

## 12. Merge order

- **Fork first.** `tools/ci-game-files` and `tools/des-release-tags` are independent of each other.
- **Then the port:**
  1. `tools/ci-game-files`
  2. `tools/ci-port`
  3. `tools/ps3recomp-pin`
  4. `feat/version-display`
  5. `tools/release-workflow`
- **Dependencies:**
  - None of them depends on another to build or pass its own checks.
  - `release.yml` needs `pin_ps3recomp.py` and `check_game_files.py` on master. Without them it stops with a clear message.
  - Once `feat/version-display` and `tools/ci-port` are both merged, the CI UI build also runs `DesVersion.cmake`. No change is needed (inferred).

## 13. Not done, and open questions

- The SPU lift's determinism.
- A full `des_boot` built in two folders and compared. That waits for the section 5.4 flags.
- No workflow has run on GitHub. The first runs may need small fixes (inferred risk). The runner image's LLVM version and Ninja from Chocolatey are not checked.
- Whether SignPath Foundation accepts a game port.
- What upstream wants to do about the old firmware files.

## 14. Run requests

**R1. Version display (runs the game).**
- **Exe:** the `feat/version-display` build (path in the lane report).
- **Run it** with `DES_SKIP_INTRO=1 PS3_SAIL_MOVIES=0`, but without `DES_AUTO_CONTINUE`.
  - With `DES_AUTO_CONTINUE` on the command line, the run counts as scripted and shows no start screen (read: `scripted_run` in `port/ui/src/settings.cpp`).
  - Add `DES_REPORT_AT_FRAME=900 DES_REPORT_NO_OPEN=1`. This saves an issue report at frame 900 without opening the browser (read: `report.hpp`).
- **Check:**
  - The start screen reads "Version 0.1.0-dev.327+gdac152274".
  - Settings > About has the rows Version, ps3recomp and Game dump. The help pane shows the full hash in four groups.
  - Near the top of the log, just before `[ui] settings:`, is the line `[des] DemonsSoulsRecomp ..., ps3recomp ..., game dump ...`.
  - The log line `[report] issue report: <zip>` names the zip. Its `system.txt` has the ps3recomp and game dump lines.
  - Explorer, Properties > Details on the exe: File version 0.1.0.327 and Product version.
  - Optional, the crash report: run again with `DES_AUTO_CONTINUE=1 DES_CRASH_AT_FRAME=900`. The crash report has the line `ps3recomp ..., game dump ...`.
- **Expect:** no change in behaviour, only text.

**R2. Two-folder build (later, build only).** After the section 5.4 flags:
- Build `des_boot` in two build folders with different paths.
- Compare the SHA-256 of the exes.
- Expect: identical.

## Sources

- GitHub Docs, workflow syntax: the filter pattern cheat sheet, and "checks associated with that workflow will remain in a Pending state" for skipped workflows. Read 2026-10-18.
- GitHub CLI manual, `gh release create`: `--draft`, `--verify-tag`, `--prerelease`. Read 2026-10-18.
- GitHub Docs, available rules for rulesets: restrict creations, updates and deletions for branches or tags. Read 2026-10-18.
- GitHub Docs, secure use reference: pin actions to a commit SHA; "a tag can be moved or deleted". Read 2026-10-18.
- Microsoft Learn, VERSIONINFO resource: four 16-bit integers; VS_FF_PRERELEASE; the required strings. Read 2026-10-18.
- docs/05, docs/45 (sections 4.5, 5.1 to 5.3, 6.1 and 9), docs/52 (sections 6 to 8).
