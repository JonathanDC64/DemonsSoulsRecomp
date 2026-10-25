# 105. Updates in the launcher (2026-10-24)

docs/45 planned updates in two stages: a check with release notes, then updates applied in the app. This doc is what
was built: both, for the Windows download. Labels: **measured** (a run or a test says so), **read** (seen in the code
or the docs), **inferred**.

## Short answer

- **At each start the launcher asks GitHub for the newest release.** It takes about a second, off the UI's thread. It
  says nothing when it gets no answer (offline, or GitHub's hourly limit).
- **A newer release shows on the start screen** as an "Update to 0.2.0" button, and on setup's own start screen as a
  note with "What's new". Nothing shows in play.
- **The update window** has the release's notes and three buttons: **Update**, **Later** and **Skip this version**.
  Update downloads the release, checks it against the release's SHA256SUMS, puts it in place, then starts the new
  version straight into setup's rebuild. Saves, settings, the shader cache and the game files are kept.
- **Only signed releases.** A release is offered and installed only if its SHA256SUMS carries the maintainer's
  signature, made offline with the project's release key and checked against the keys the installed version lists
  (section 7). A release that is not signed is never shown, so its notes are never read either.
- **One version is kept to go back to.** Settings, About, Updates has "Go back to 0.1.0": the program files and the
  game build of that version come back, so the game starts at once, with no rebuild.
- **Settings, About, Check for updates** turns the check at the start off. "Check now" looks once.
- **Never in test runs or developer builds.** Only a release (a plain X.Y.Z version) that runs from a player's install
  checks.

## 1. What the player sees

| Where | What |
|---|---|
| The start screen (the game is built) | "Update to X" between Trophies and Quit, once the check found X and it is not skipped. |
| Setup's start screen (setup alone) | A note "Version X is out" and a "What's new" button, in every state. |
| Settings, About | "Check for updates" (on by default); "Updates" with a status line, then Check now, What's new in X, Go back to Y. |
| The update window | The notes (headings, list items and lines from the release's Markdown), Release page, Update, Later, Skip this version. |

The flow (read: `update_window.cpp`, `des_ui.cpp`):

1. **Update over the game.** A running game cannot replace its own files. So the program starts again with setup's
   screens alone, open on the update (`DES_UI_OPEN=update-run`, `DES_UPDATE_TO=X`), and the game closes.
2. **Update on setup's screens alone** runs at once.
3. **The update runs:** a bar, "Downloading, 12 of 40 MB", then "Putting it in place". Stop is there only while it
   downloads. What is downloaded is kept, so the next try carries on.
4. **At the end** the program starts again from the same file, now the new version's, with `DES_UI_OPEN=setup-run`:
   setup's progress screen, rebuilding the game for the new version with the folders the last run used. The rebuild
   is setup's own run: a phase whose inputs did not change is skipped (docs/88).
5. **If it fails,** nothing was changed. The page says what failed, with Try again and Back.

Going back is the same, from Settings, About: a question first, then `update-back`. At its end the program starts
again as the older version, which finds its own build and plays. The version left is marked skipped, so the start
screen does not offer it straight back.

## 2. Settings and knobs

| Name | What |
|---|---|
| `update_check` (settings.ini) | 1 (default): check at each start. 0: only Check now. |
| `update_skip` (settings.ini) | The release the player chose to skip. Set by Skip this version, and by going back. |
| `DES_UI_OPEN=update-run` | Setup's screens alone, on the update to `DES_UPDATE_TO`. The start screen's Update uses it. |
| `DES_UI_OPEN=update-back` | Setup's screens alone, going back. Settings' Go back over the game uses it. |
| `DES_UPDATE_FEED` | Tests: the releases list from a file or another URL. Downloads may then come from `http://127.0.0.1`. It also allows the check in a test run. |
| `DES_UPDATE_AS_VERSION` | Tests: this program counts as that version (a developer's build never checks otherwise). After an update the new start counts as the new version. |

## 3. The check

`update-check --app VERSION` (read: `tools/setup/des_setup/update.py`):

- **The list:** `GET https://api.github.com/repos/JonathanDC64/DemonsSoulsRecomp/releases?per_page=20`. The answer
  and its ETag are kept in `<data>/setup/update/feed.json`. An unchanged list then answers 304, which does not count
  against GitHub's limit of 60 requests an hour without a token (read: GitHub's REST docs).
- **What counts:** published releases only (not drafts or pre-releases), with a `vX.Y.Z` tag, a player's download for
  this system (`DemonsSoulsRecomp-X.Y.Z-windows.zip`, or on Linux `DemonsSoulsRecomp-X.Y.Z-linux.tar.xz`),
  `SHA256SUMS` and `SHA256SUMS.sig`. So `llvm-22.1.8-1`, setup's compiler release,
  never counts, though GitHub marks it Latest today.
- **Signed:** for the newest newer release the check downloads `SHA256SUMS` and `SHA256SUMS.sig` (a few hundred
  bytes) and checks the signature (section 7). One that fails is passed over with a `note version X is not offered:
  <why>` line, and the next newer one is tried, at most 3.
- **Compared as numbers:** 0.2.10 is newer than 0.2.9. Only a plain X.Y.Z build compares, so a developer's build
  (0.1.0-dev.5+g1234) finds nothing.
- **Lines for the host:** `@release <version> <time> <page>`, the notes as `@note` lines (at most 300, each at most
  500 characters), then `@done ok`. Nothing newer: `@done ok` alone. No answer: `@error download - <sentence>` and
  `@done failed`. The protocol stays version 2: the host ignores keywords it does not know (protocol.py).
- **The host starts it** at the UI's start when `updates::wanted()` (`port/ui/src/updates.cpp`): a release version, a player's install
  (`release-info.txt` beside the exe, no `.git`), the engine and its Python there (setup fetches the Python at its first
  run, so a first start before setup does not check), not a test run, and `update_check` on.

## 4. The update

`update-apply --app OLD --to NEW --install DIR` (read: `update.py`; measured: `tests/test_update.py`):

1. **Lock:** setup's `run.lock`, so no setup run and no other update runs at the same time.
2. **Checks:** DIR is a player's install (`release-info.txt` and the exe at its top, no `.git`) and can be written to.
3. **Download:** `SHA256SUMS` (at most 64 KB) and `SHA256SUMS.sig` (at most 16 KB), the signature checked
   (section 7; a failure is `@error signature download`, and nothing is downloaded further), then the zip, by fetch.py: resumed with HTTP Range, its size and
   SHA-256 checked before it is used. A zip that does not match is deleted. Free space first: three times the zip, plus
   the game build's size, on both the install's drive and the data folder's.
4. **Unpack** into `DIR/.update/NEW`, with fetch.py's checks (no absolute paths, no `..`, no links). The zip's
   `DemonsSoulsRecomp-NEW/` must hold the exe and `tools/setup/run_setup.py`.
5. **The game build is copied** into `<data>/previous/OLD`: `game/` (the module, about 280 MB) and `setup/ready.txt`.
   A copy, not a move: the rebuild starts from the module there, and a version whose game code did not change keeps
   it (its journal steps are all skipped).
6. **The swap.** The program files are the names at the top of the install that `release-files.txt` lists (the
   player's zip has it, written by `package.py`). Only plain names count: a line with `..`, a separator (`/` or
   `\`), a drive colon or a control character names nothing. An install from before that file: the names the new release has.
   Each of them moves into `DIR/.previous/OLD`, then the new release's names move in. The exe moves out last and in
   first, so the install has none for the shortest time. The old `.previous` is renamed to `.previous.old` first and
   deleted only once the swap is done.
7. **Clean up:** `.update/NEW`, the zip, and the version kept before go. One version is kept.

Renames on the same drive are quick; Windows lets a running exe be renamed (measured in the end-to-end run, section
9). A rename that fails on a sharing error is tried again for 4 seconds (a virus scanner reading a new file), then the
swap is undone and the page says "A file in the game's folder is in use".

## 5. Going back

`update-back --app CUR --install DIR`: the same swap the other way.

- `.previous/OLD` moves to `.update/OLD`, then the swap puts CUR's program files into `.previous/CUR` and OLD's in place.
- The game build: `<data>/game` and `ready.txt` move into `<data>/previous/CUR`; OLD's move back.
- The journal's `link` step is forgotten. It made CUR's module, which is now aside, so a later rebuild for CUR links
  again rather than keep OLD's module.

## 6. What moves and what stays

| | Update | Go back |
|---|---|---|
| Program files (`release-files.txt`'s names) | to `.previous/OLD`, NEW's in place | swapped back |
| `gamedata/` (saves), `shader_cache/`, `data/` and `portable.txt` (a portable install's settings) | stay | stay |
| Anything else at the install's top | stays | stays |
| Settings (`%APPDATA%\DemonsSoulsRecomp`) | stay | stay |
| Setup's data folder: `inputs/`, `work/`, `build/`, `tools/`, `downloads/`, `hdd1/` | stay; the rebuild updates `work/` and `build/` | stay |
| `<data>/game` and `setup/ready.txt` | copied into `previous/OLD`; the rebuild replaces them | swapped with `previous/OLD` |

## 7. Safety

- **The swap's journal.** Each rename is written to `DIR/.update/swap.txt` before it is made: lines of tab-separated
  fields, `made <folder>` and `move <from> <to>`. A swap that fails is undone from the last line up at once.
- **A swap cut short** (a power cut) is undone at the program's next start, by the host itself
  (`updates::recover_swap`, read in `des_ui_launch` before anything else), since the engine may be half swapped too.
  Both sides undo a journal only if every path in it is inside the install; one that names anything else is left
  as it is (the engine says so and stops the update).
  The host then starts the program that is in place and ends. The next update-apply or update-back would undo it as
  well.
- **The close button does nothing while the files go in place** (the install step: the unpack, the copy of the game
  build and the swap, a few seconds): ending the program then would end the engine half way (`updates::hold_close`,
  des_ui.cpp's WM_CLOSE).
- **Still possible:** a power cut in the two renames between the old exe out and the new exe in leaves no exe at the
  top. Extract the zip again over the folder; nothing of the player's is lost.
- **Downloads** are https only (fetch.py), checked by SHA-256 against the release's SHA256SUMS, and SHA256SUMS is
  checked against its signature first (below). So an account or a token that can change a release cannot ship code
  to players: it would need the release key, which never goes online.

### 7.1 Signed releases

- **The key:** an Ed25519 key in OpenSSH's format, made and kept by the maintainer, offline, with a passphrase. Its
  public half is a line in `tools/setup/des_setup/release-keys.txt`, which ships in every release. With no key there
  the build installs no update.
- **The signature:** `ssh-keygen -Y sign -n demons-souls-recomp-release` over `SHA256SUMS`, uploaded beside it as
  `SHA256SUMS.sig` (OpenSSH's SSHSIG format; the namespace keeps a signature made for anything else from counting).
  `tools/release/sign_release.py` does it: it downloads the draft, checks each file against SHA256SUMS and against
  its build attestation (release.yml attests each file SHA256SUMS lists), signs, checks the signature with the
  engine's own code against the key lists at the tag and at the release before it, and uploads it.
- **The check:** `tools/setup/des_setup/signing.py`, in plain Python (setup's Python has no Ed25519): the signature's
  key must be one of the installed version's keys, the namespace and hash must be the expected ones, and Ed25519's
  check (RFC 8032) must pass. Measured: about 10 ms. Its tests check RFC 8032's vectors, a signature made by
  Windows' own `ssh-keygen` (OpenSSH 9.5), and signatures `ssh-keygen -Y verify` accepts.
- **The installed version decides:** a release cannot vouch for itself. To change keys, add the new key's line in
  one release (signed with the old key), then sign later ones with the new key. A lost key means players update by
  hand once, to a release that lists a new one.

The maintainer's steps, once:

1. Make the key, on a PC you trust, with a passphrase you keep somewhere safe:
   `ssh-keygen -t ed25519 -C "DemonsSoulsRecomp releases" -f <folder>\dss-release`
   This writes `dss-release` (private: never share it, never commit it) and `dss-release.pub`.
2. Keep a copy of the private file offline (for example on a USB stick in a drawer). Without it no later release
   can be signed for the players who have this key.
3. Put the one line of `dss-release.pub` in `tools/setup/des_setup/release-keys.txt`, in a PR, before the first
   release. The public line is fine to share and to commit.
4. In the repository's settings, Environments, add `release` and make it require your review, so a tag pushed by
   anyone else drafts nothing until you approve it. In Settings, General, turn on immutable releases if offered.

For each release: after release.yml has drafted it, run
`python tools/release/sign_release.py vX.Y.Z --key <folder>\dss-release`, type the passphrase when ssh-keygen asks,
then read the draft and publish it (docs/54 section 8.4).

## 8. Tests

- **Signatures** (`tests/test_signing.py`, `tests/test_sign_release.py`, measured: all pass): RFC 8032's vectors,
  an OpenSSH-made signature, refusals (another key, another namespace, a changed file, broken armour, no key), the
  key list; the signing script with gh, git and ssh-keygen stood in for (a file that does not match, no
  attestation, a published release, a key the version before does not list).
- **Unit tests** (`tools/setup/tests/test_update.py`, 16 tests, measured: all pass): releases signed with a key the
  build does not list are neither offered nor installed, a build with no key installs nothing, release-files.txt
  names that would leave the install and a journal path outside it are refused, and the release filter (drafts,
  pre-releases, the llvm release, a release without SHA256SUMS, 0.2.10 over 0.2.9, developer builds), the check's
  lines, an update and then going back on a made-up install served from 127.0.0.1 (player files kept, the build kept
  and restored, the journal's link forgotten), a bad checksum (nothing changed), a file in use half way (the swap
  undone), a swap cut short (undone), an install without `release-files.txt`, a developer's tree (refused), and
  `release-files.txt` itself.
- `test_pipeline.py`'s host test checks that the host's parser knows `@release` and `@note`.
- **End-to-end:** see section 9.

## 9. End-to-end run

Measured 10-24 on this branch's own package, in a scratch install with its own data folder and settings:

- **The setup.** The branch's player zip unpacked as the install (version 0.1.0 for the test). A second zip made from
  it as 0.2.0, with one marker file added (`docs/NEW-IN-0.2.0.md`) and its SHA256SUMS, served from `127.0.0.1` by
  `python -m http.server`. A feed file named it, beside a draft, a pre-release and an `llvm-` release that must not
  count (`DES_UPDATE_FEED`, `DES_UPDATE_AS_VERSION=0.1.0`). The data folder: a copy of a real setup's, built once by
  this exe (`DES_UI_OPEN=setup-run`).
- **The check.** At the game's start: `[update] version 0.2.0 is out (this is 0.1.0)`, once. The start screen showed
  "Update to 0.2.0" between Trophies and Quit; its notes window showed the headings, the list items (one nested) and
  the plain text of a bold, code and link line, with Update, Later, Skip this version and Release page.
- **The update,** pressed on the start screen with keys sent through the debug console: the program started again on
  `update-run`; SHA256SUMS and the zip were downloaded at 21:30:17; the swap was done at 21:30:18; the new program's
  rebuild ended at 21:30:22 with every phase skipped but the shaders (0.6 s), as the game code had not changed. After
  it: `release-info.txt` said 0.2.0, the marker file was in place, `.previous/0.1.0` held the old program files,
  `<data>/previous/0.1.0` held `game/` and `ready.txt`, and `.update` was empty. The new program showed "The game is
  built".
- **Going back** (`DES_UI_OPEN=update-back`, as 0.2.0): done in under two seconds. `release-info.txt` said 0.1.0
  again, `.previous/0.2.0` and `<data>/previous/0.2.0` were kept, `update_skip = 0.2.0` was in settings.ini, and the
  program started again straight into the game's start screen, with no Update button.
- **Windows let the running exe be renamed** (section 4): the update moved `des_host.exe` while the program it ran
  (setup's screens, which started the update) was still open.
- **What it does not show:** the restarted programs' own lines, which go to no console (the restart does not hand
  them its standard error). Setup's `logs/run.log` in the data folder has the rebuild's.

## 10. Contracts for later versions

An update starts the new version's program with what the old one passes, so these must keep working in every later
version:

- `DES_UI_OPEN=setup-run` opens setup's progress screen and runs setup at once, with the last run's folders.
- The player's zip is `DemonsSoulsRecomp-X.Y.Z-windows.zip` with `DemonsSoulsRecomp-X.Y.Z/` inside, the exe
  `des_host.exe` and `release-files.txt` at its top, beside `SHA256SUMS` in the same release. On Linux it is
  `DemonsSoulsRecomp-X.Y.Z-linux.tar.xz` (`package.py --linux-host`), the same layout with `des_host` (a tar keeps
  its execute bit; `fetch.unpack` takes both).
- `SHA256SUMS.sig` beside `SHA256SUMS`, an SSHSIG in the namespace `demons-souls-recomp-release`, by a key the
  previous version's `release-keys.txt` lists.
- `update-back` reads `.previous/<version>` and `<data>/previous/<version>`, as written here.

## 11. Later

- **Linux:** done on the update side: `player_asset` names the tar.xz, and the POSIX `start_again` starts the path
  the program started from. `/proc/self/exe` follows the running file, so after a swap it named `.previous/<version>`
  and started the old version again (checked with a stand-in in WSL); `report::program_path()` now reads it once at
  the start. The package itself is the Linux setup branch's (`package.py --linux-host`, host.yml's `host-linux`
  job), so Linux updates start with the first release that has both. Once both are in, the Linux tarball should
  carry `release-files.txt` too, as the zip does (one line in `cmd_build`); until then an update swaps the names
  the new release has.
- **Signing the exe** (Authenticode, docs/45 section 5): Windows' own trust in the program, apart from the
  update's.
- **Smaller downloads:** today an update is the whole zip (11.6 MB for this branch's package, 1,076 files, measured).
  docs/45 has the component plan.
- **A beta channel** (pre-releases) behind a setting.
