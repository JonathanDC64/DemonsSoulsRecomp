# RPCS3 issues for Demon's Souls: what might apply to the port

Date: 2026-10-17.

This is a survey of RPCS3 issues and PRs about Demon's Souls (DeS). The aim
is to find fixes that might also apply to the static-recompilation port
(ps3recomp: lifted PPU/SPU code, our own HLE modules, SPU MFC/atomics, RSX
decoder, D3D12 renderer). No code was changed for this survey.

Sources:
- GitHub issue and PR search on RPCS3/rpcs3 for "Demon's Souls", "Demon Souls",
  "DeS" and the game's serials (BLUS30443, BLES00932, BCJS30022, BCAS20071,
  NPUB30910, NPJA00102 and others).
- For each closed issue: the body, the comments, and the closing PR or commit
  (GitHub timeline cross-references).
- 135 PRs that mention DeS in their body or comments.
- The RPCS3 source (a recent shallow clone) for current behaviour.
- A local copy of RPCS3's `patch.yml`.
- Our side: docs/28, and the ps3recomp sources.
- Not reachable: the RPCS3 wiki page for DeS. It sits behind a Cloudflare
  challenge, so it could not be fetched.

Counts: 218 issues found (199 closed, 19 open).
- 11 apply ("yes"). Most of these we already have.
- 20 might apply ("maybe").
- 187 do not apply. The 19 open issues are in this group: they have no fix to copy.
- PR-only items (no DeS issue behind them) are listed with the candidate they
  belong to.

Rules used here: each entry states what the issue or PR says. Anything I
inferred is marked **guess**. "Have it" means the port already has the fix or
already hit and fixed the same problem.

## Look into first

1. **cellSysCacheMount: reuse the cache (RELAYED).** Ours always returns
   "cleared". DeS builds a big /dev_hdd1 cache, and RPCS3 notes it "can take
   minutes to boot without a cache". Small change; payoff is boot time.
2. **cellSaveData write path.** RPCS3 had DeS autosave stutter (#9857), save
   crashes around temp-directory moves (#5481), and a half-second save
   completion delay that DeS handles fine (PR #13492). Ours writes in place,
   with no temp file and no flush. Medium; payoff is save safety, and avoiding
   autosave hitches if we ever add a flush.
3. **lv2 sync primitives audit.** Several DeS freezes were lwcond, cond or
   mutex races: PRs #12496, #12482, #5667, #8340, #5475, #6604. These show up
   in long sessions and at level warps. Medium; payoff is fewer rare hangs.
4. **cellGcmGetLastFlipTime units.** DeS imports it. Ours returns nanoseconds;
   RPCS3 returns microseconds. Wrong flip-time units gave DeS black FMVs in
   RPCS3 (#12118, PR #12120). Small; the payoff is unknown, since our FMVs
   already play.
5. **Two cheap checks.** Colour-mask writes with reserved bits (PR #14545: real
   hardware discards them; ours decodes any nonzero byte as "on"). And
   cellSail playback after Quit Game and in the end credits (#3476, #4302).
   Small; correctness.

## Follow-up, 2026-10-18

The five items above, checked against the game:

1. **cellSysCacheMount: does not apply.** DeS mounts the cache with an
   empty id (`SysCacheMount(id='')`, from 0x00C63E88). RPCS3 never reuses a
   cache with an empty id either ("won't if cache id is an empty string"),
   and returns OK_CLEARED like ours. So DeS rebuilds its /dev_hdd1 cache on
   every boot in RPCS3 as well, and there is no boot time to win here.
2. **cellSaveData write path: fixed** (ps3recomp PR #48). A whole-file write
   now goes to `.<name>.tmp` and replaces the file once it is complete, so a
   crash mid-autosave keeps the old USER.DAT. No flush was added. RPCS3's
   other DeS save issue (the half-second completion delay) needs nothing
   from us.
3. **lv2 sync primitives audit: not done.** Still open; it is a longer job.
4. **cellGcmGetLastFlipTime: does not matter for DeS.** DeS calls it in one
   place (0x00FE0378) and passes the value straight to
   `cellSailGraphicsAdapterUpdateAvSync`, which our cellSail ignores. The
   units are still wrong for other games (ours: host ns; RPCS3: guest µs,
   minus one second), but changing them is untested anywhere, so it is
   left alone.
5. **Colour mask reserved bits: fixed** (ps3recomp PR #47), as RPCS3 does:
   a COLOR_MASK write with any bit other than bit 0 of each byte is ignored.
   Our D3D12 path decoded bit 0 only, so such a write turned channels off
   (not on, as the item above guessed). DeS sends none (0 in a 120 s level
   run). cellSail after Quit Game and in the credits was not checked; it
   needs a run that gets that far.

## Candidates in detail: yes

### Y1. S1 normalised vertex attribute: missing shields, eggs, hoods (have it)
- Issues: [#3088](https://github.com/RPCS3/rpcs3/issues/3088) "Soldier shield
  not being drawn", [#4302](https://github.com/RPCS3/rpcs3/issues/4302)
  "Demon's Souls Glitches", [#5153](https://github.com/RPCS3/rpcs3/issues/5153)
  "Missing object in Demon Souls 7367".
- Fix: PR [#12450](https://github.com/RPCS3/rpcs3/pull/12450) "rsx/vp: Fix
  Demon Souls missing graphics".
- Symptom: missing objects. Per #4302: the Latria 3-2 egg/bubble, a boss hood,
  the 4-2 cave floor, the round shield, and Storm King's projectiles.
- Area: RSX/GPU.
- Applies: yes. Same decode bug class.
- Status: have it (ps3recomp PR #19).
- Effort / payoff: done.

### Y2. Sun flare through walls (ZCULL / ZPASS reports) (have it)
- Issues: [#3643](https://github.com/RPCS3/rpcs3/issues/3643) "Sun Glare / Lens
  Flare effect indoors", [#7213](https://github.com/RPCS3/rpcs3/issues/7213)
  "Sun shines through solid objects", and the same again in
  [#12635](https://github.com/RPCS3/rpcs3/issues/12635).
- Fixes: PRs [#7216](https://github.com/RPCS3/rpcs3/pull/7216),
  [#10823](https://github.com/RPCS3/rpcs3/pull/10823) "rsx: Implement accurate
  ZCULL stats", and [#12645](https://github.com/RPCS3/rpcs3/pull/12645)
  "rsx: Properly reset ZCULL stat counters".
- Symptom: the sun flare draws indoors and through objects.
- Area: RSX/GPU.
- Applies: yes.
- Status: have it. `rsx_d3d12_backend.c` turns ZPASS reports into D3D12
  occlusion queries (`RSX_ZPASS` knob). Its comment says answering "visible"
  drew the flare through walls. One lesson from #12645 to keep: reset the
  counters on CLEAR_REPORT_VALUE.
- Effort / payoff: done.

### Y3. Exposure needs CPU reads of colour buffers (have it)
- PRs [#3664](https://github.com/RPCS3/rpcs3/pull/3664) "rsx: Fixes",
  [#5146](https://github.com/RPCS3/rpcs3/pull/5146) "rsx: Fixes" (Valley of
  Defilement too dark), and [#5276](https://github.com/RPCS3/rpcs3/pull/5276).
- The patch.yml note for DeS says: "Can disable Write Color Buffers for 20%
  faster performance ... gives a slightly darker tone to the game."
- Symptom: the scene is too dark or too bright without colour write-back.
- Area: RSX/GPU.
- Applies: yes.
- Status: have it. docs/28 covers colour write-back (c403bd9) and the
  luminance/exposure fix.
- Effort / payoff: done. The 20% figure suggests the write-back is worth
  keeping cheap (**guess** about our cost).

### Y4. cellSysCacheMount RELAYED and the /dev_hdd1 cache
- Issues: [#3239](https://github.com/RPCS3/rpcs3/issues/3239) (a hang that
  survived deleting /data; kd-11: "DeS writes some cached files to /dev_hdd1")
  and [#3646](https://github.com/RPCS3/rpcs3/issues/3646) (kd-11: "The game
  generates the cache itself from game files"; the first screen takes a long
  time while it does).
- PRs: [#4848](https://github.com/RPCS3/rpcs3/pull/4848) "Fix cellSysCacheClear".
  It fixes the return codes of cellSysCacheMount; a comment says DeS should
  get "similar fixes".
- [#5411](https://github.com/RPCS3/rpcs3/pull/5411) "Clear disk cache if it
  grows too large". A comment says: "Some games (like DeS) can take minutes to
  boot without a cache, but less than 5 seconds with one available."
- Current RPCS3 (`cellSysCache.cpp`): returns `CELL_SYSCACHE_RET_OK_RELAYED`
  when the cache ID matches the one kept from last time. Otherwise it clears
  the old cache and returns OK_CLEARED. The mount path is `/dev_hdd1`.
- Ours (`ps3recomp/libs/system/cellSysutil.c`, `cellSysCacheMount`): always
  returns OK_CLEARED (0), with path `/dev_hdd1/cache/<id>`. It never returns
  RELAYED.
- Area: HLE module.
- Applies: yes. The game decides whether to rebuild from the return code
  (**guess** about DeS's exact logic).
- Status: unknown. I could not tell from the code whether the game rebuilds
  its cache every boot under our port. Worth timing a second boot.
- Effort: small. Payoff: boot time, maybe large; and a defined way to recover
  from a bad cache (clear on ID change).

### Y5. cellGcmGetLastFlipTime returns ns, RPCS3 returns µs
- Issue: [#12118](https://github.com/RPCS3/rpcs3/issues/12118). A God of War
  issue; testers report DeS intro and title-screen videos were black from the
  same regression.
- Fix: PR [#12120](https://github.com/RPCS3/rpcs3/pull/12120) "rsx: Fix
  cellGcmGetLastSecondVTime, revert FLIP time fix". It says flip and vblank
  times clock at 80 MHz (the MFTB rate), and that cellGcmGetLastFlipTime
  converts to microseconds.
- RPCS3 today: `last_guest_flip_timestamp = get_system_time() - 1000000`
  (microseconds).
- Ours (`ps3recomp/libs/video/cellGcmSys.c`): `s_last_flip_time =
  get_timestamp_ns()`, which is nanoseconds. The DeS lift imports this
  function (NID 0x63387071). Also, the comment above our function names NID
  0x21AC3697, which is cellGcmAddressToOffset.
- Area: HLE module.
- Applies: yes. Units differ by 1000x.
- Status: our FMVs already play, so DeS may only use it for pacing or
  statistics (**guess**).
- Effort: small. Payoff: unknown; low risk.

### Y6. cellSaveData: autosave stutter and save crashes
- [#9857](https://github.com/RPCS3/rpcs3/issues/9857): stutter or freezes when
  switching inventory menus, picking up items, and walking in some archstones.
  This was a regression from PR [#9819](https://github.com/RPCS3/rpcs3/pull/9819)
  "Atomic PARAM.SFO modifications", which flushed temp files to disk before
  renaming them. Fixed by PR [#10479](https://github.com/RPCS3/rpcs3/pull/10479)
  "fs: alternative fs::pending_file implementation (Win32)".
- [#5481](https://github.com/RPCS3/rpcs3/issues/5481) "Demon Souls save crash".
  Fixed by PR [#5614](https://github.com/RPCS3/rpcs3/pull/5614) "Try to fix
  random save data failure".
- PR [#13492](https://github.com/RPCS3/rpcs3/pull/13492) "Slow save completion".
  The save thread now sleeps about 0.5 s, because some games miss a save that
  completes too fast. Its comments say DeS autosaves every few seconds, and
  slow save I/O once made it "unplayable". DeS was tested with the PR: the
  save icon shows and there is no stutter.
- PR [#17330](https://github.com/RPCS3/rpcs3/pull/17330): a sort comparator
  fix in savedata.
- Ours (`ps3recomp/libs/system/cellSaveData.c`): files are written in place.
  There is no temp file, rename or flush.
- Area: HLE module.
- Applies: yes.
- Status: we are not exposed to the #9857 stutter, because we never flush.
  A crash in the middle of a save could leave it half-written (**guess**).
  Not checked: whether DeS's save-icon logic expects a slower completion.
- Effort: medium. Payoff: save safety. If we add atomic writes, do the flush
  off the game thread, or use a cheap rename, to avoid #9857.

## Candidates in detail: maybe

### M1. lv2 sync-primitive races behind DeS freezes
- [#5654](https://github.com/RPCS3/rpcs3/issues/5654): infinite load when
  warping to Latria 3-1. Fixed by PR
  [#5667](https://github.com/RPCS3/rpcs3/pull/5667) "Fix race in
  sys_lwcond_wait on error code".
- [#3634](https://github.com/RPCS3/rpcs3/issues/3634): a sys_cond_wait mutex
  ownership check failing in `ChrProxyThreadMan::Thread`. A comment says
  "Fixed by #5314".
- [#4861](https://github.com/RPCS3/rpcs3/issues/4861): freezes at the logo or
  intro on 4-core/4-thread CPUs. The reporter says PR
  [#5475](https://github.com/RPCS3/rpcs3/pull/5475) fixes it (a mutex
  destroyed while an unlock is still running). PR
  [#5310](https://github.com/RPCS3/rpcs3/pull/5310) (thread group join waits
  for the threads to stop) is also linked.
- [#8331](https://github.com/RPCS3/rpcs3/issues/8331): infinite loading.
  Fixed by PR [#8340](https://github.com/RPCS3/rpcs3/pull/8340), which writes
  the SPU inbound mailbox under the mutex. The author: "Found the race
  condition!"
- PR [#6604](https://github.com/RPCS3/rpcs3/pull/6604): a signal flag left set
  after a sys_cond_wait ETIMEDOUT. "Seen in Demon Souls."
- PRs [#12482](https://github.com/RPCS3/rpcs3/pull/12482) "LV2 Fixups" and
  [#12496](https://github.com/RPCS3/rpcs3/pull/12496) "Stability fixes"
  (a loose notification in lwcond wait: wake under the mutex lock). The author:
  "For me it fixes freezes in Demon Souls after a while."
- Area: sync primitives.
- Applies: maybe. These are RPCS3 scheduler races, but our sys_lwcond,
  sys_cond and SPU mailbox code face the same orderings. Our sys_mutex already
  returns EDEADLK, as DeS expects (see #8791's log).
- Status: unknown. Our Nexus freeze was a lwarx ordering bug, not one of these.
- Effort: medium (audit our code against each race). Payoff: rare long-session
  and warp hangs.

### M2. cellSail flicker after Quit Game, white credits screen
- [#3476](https://github.com/RPCS3/rpcs3/issues/3476): the menu flickers after
  Options, Quit Game, and during the end titles. kd-11: "Classic cellSail
  issue."
- #4302 also mentions the credits showing as a white screen.
- Area: HLE module (cellSail) and RSX.
- Applies: maybe. We have our own cellSail.
- Status: unknown. Quit-to-menu and the credits have not been tested
  (**guess** from docs/28, which does not mention them).
- Effort: small to test. Payoff: correctness at the end of the game.

### M3. Colour-mask reserved bits
- PR [#14545](https://github.com/RPCS3/rpcs3/pull/14545) "rsx: Discard color
  mask writes with reserved bits". It is based on a hardware test. Its first
  version broke graphics in almost every game, DeS included; a retest after a
  fix rendered properly.
- Ours (`rsx_live_draw.c`, `apply_render_state`): any nonzero byte turns that
  channel on. Reserved bits are not checked.
- Area: RSX/GPU.
- Applies: maybe. It only matters if DeS writes masks with reserved bits.
- Status: unknown.
- Effort: small (log any mask with reserved bits). Payoff: small.

### M4. Default vertex input is (0,0,0,1)
- PR [#6485](https://github.com/RPCS3/rpcs3/pull/6485) "rsx: Minor shader
  generation improvements", linked from #4302.
- Area: RSX/GPU.
- Applies: yes in principle.
- Status: appears to be in place. `rsx_fetch_attrib` reads a disabled array
  from the constant register, and `rsx_commands.c` starts it with w = 1.0.
  The VP decompiler sets inputs and outputs to (0,0,0,1).
- Effort / payoff: none needed.

### M5. Surface and texture cache: crashes at level load, warp and fog gates
- PR [#11701](https://github.com/RPCS3/rpcs3/pull/11701): "Fixes DeS crashing
  on loading levels. Fromsoft engine has some quirky PS3 behavior ... dynamic
  image usage ... in-place swapping of locked regions."
- [#8692](https://github.com/RPCS3/rpcs3/issues/8692): crashes when warping or
  entering fog gates, plus object glitches. Closed by PR
  [#8696](https://github.com/RPCS3/rpcs3/pull/8696) "Improve swizzled layout
  detection" and PR [#8694](https://github.com/RPCS3/rpcs3/pull/8694) (FMA
  signed zero).
- [#9509](https://github.com/RPCS3/rpcs3/issues/9509): broken dynamic shadows.
  Fixed by PR [#9512](https://github.com/RPCS3/rpcs3/pull/9512).
- Area: RSX/GPU.
- Applies: maybe. It depends on how our renderer tracks render targets that
  are reused in place.
- Status: our shadows work. docs/28 records no crash at warps or fog gates;
  later levels are not fully tested (**guess**).
- Effort: medium if a problem appears. Payoff: crash safety on later levels.

### M6. Audio: mixer death, crackle, sampling skip
- [#2507](https://github.com/RPCS3/rpcs3/issues/2507): kd-11 says DeS freezes
  are "often preceded by the death of the audio mixer".
- [#4381](https://github.com/RPCS3/rpcs3/issues/4381): freezes and audio
  stutter. Closed as fixed by PR
  [#5456](https://github.com/RPCS3/rpcs3/pull/5456) "Audio Buffering", which
  says PS3 audio runs in fixed 5.33 ms periods.
- [#12833](https://github.com/RPCS3/rpcs3/issues/12833): crackle. Fixed by PR
  [#12837](https://github.com/RPCS3/rpcs3/pull/12837), a setting to disable
  sampling skip.
- PR [#4020](https://github.com/RPCS3/rpcs3/pull/4020) (closed): removing the
  `_mxr000` hack lost DeS audio. DeS relies on that mixer event-queue
  behaviour.
- Area: HLE module (cellAudio) and sync primitives.
- Applies: maybe.
- Status: our audio works. Open RPCS3 issue #17167 (echoing audio) has no fix.
- Effort: small to medium. Payoff: robustness under load. With DES_FPS above
  60, the audio period stays at 5.33 ms (**guess** that ours keeps it).

### M7. Online (RPCN)
- [#8952](https://github.com/RPCS3/rpcs3/issues/8952) (two PCs on one network),
  [#8954](https://github.com/RPCS3/rpcs3/issues/8954) (sessions not working),
  and [#10473](https://github.com/RPCS3/rpcs3/issues/10473) (FPS drops online).
- PRs: [#8663](https://github.com/RPCS3/rpcs3/pull/8663),
  [#8836](https://github.com/RPCS3/rpcs3/pull/8836),
  [#12722](https://github.com/RPCS3/rpcs3/pull/12722),
  [#6661](https://github.com/RPCS3/rpcs3/pull/6661) (sceNpUtilCmpNpId;
  DeS calls it very often), and [#4204](https://github.com/RPCS3/rpcs3/pull/4204)
  (sceNpMatching2Init returns CELL_OK).
- Area: HLE module (sceNp).
- Applies: maybe, later. Online is in the future-features backlog.
- Status: not started.
- Effort: large. Payoff: online play.

### M8. Trophies
- [#8163](https://github.com/RPCS3/rpcs3/issues/8163): a freeze at "checking
  trophy set". Deleting the trophy data fixed it. A comment says the same
  happens on a real PS3.
- PR [#17077](https://github.com/RPCS3/rpcs3/pull/17077) "Fix
  sceNpTrophyRegisterContext": DeS trophy setup now shows progress correctly.
- Area: HLE module (sceNpTrophy).
- Applies: maybe, for the future trophies feature.
- Status: unknown.
- Effort: small to medium. Payoff: trophies.

### M9. Other versions and packaging
- PSN NPUB30910: [#2908](https://github.com/RPCS3/rpcs3/issues/2908) (thinks
  it is the disc game) and PRs [#8883](https://github.com/RPCS3/rpcs3/pull/8883),
  [#10849](https://github.com/RPCS3/rpcs3/pull/10849),
  [#4239](https://github.com/RPCS3/rpcs3/pull/4239) and
  [#12379](https://github.com/RPCS3/rpcs3/pull/12379).
- [#13436](https://github.com/RPCS3/rpcs3/issues/13436): NPJA00102 will not
  boot with the 1.04 update. Fixed by PR
  [#14159](https://github.com/RPCS3/rpcs3/pull/14159) "Force inherit
  BDVD/PS3_GAME across game updates".
- [#9620](https://github.com/RPCS3/rpcs3/issues/9620): a request for a
  BCAS20071 1.04 patch (now in patch.yml).
- Area: game patch and loader.
- Applies: maybe, for the future ROM-to-build patcher if it takes other
  regions or PSN copies.
- Status: we target one version.
- Effort: medium. Payoff: more users.

### M10. White lines at non-integer resolution scale
- [#3822](https://github.com/RPCS3/rpcs3/issues/3822): white lines or squares
  above 100% resolution scale. A comment says the PC Dark Souls had the same
  lines at high resolution.
- Area: RSX/GPU.
- Applies: maybe, for future display options.
- Status: we render at native resolution today (**guess**).
- Effort: unknown. Payoff: higher-resolution output.

### M11. Watch list (closed without a fix)
- [#13568](https://github.com/RPCS3/rpcs3/issues/13568) "Floating Debris":
  debris from crates that stay broken floats after an area reloads. kd-11
  could not reproduce it; a user posted a new screenshot after it was closed.
  Could be physics state on reload (**guess**). Worth a look once physics runs
  above 60 fps.
- [#11262](https://github.com/RPCS3/rpcs3/issues/11262): diagonal stick input
  sends rolls the wrong way. The reporter says squircle settings did not help.
  Relevant to our input mapping and the 8-direction backlog item.
- Area: game behaviour and input.
- Effort: unknown.

### PR-only items (no DeS issue behind them)
- PR [#17796](https://github.com/RPCS3/rpcs3/pull/17796) "SPU: Fix
  SPU_WrEventAck" (closed, not merged). It says DeS and Dark Souls write 0 to
  WrEventAck, and that on hardware this does not raise the RdEventStat count,
  so the SPU can sleep. Testers saw DeS get "marginally worse". Our
  `SPU_WrEventAck` treats a write of 0 as a no-op already. Low priority.
- PR [#12209](https://github.com/RPCS3/rpcs3/pull/12209): AVX2/AVX-512 paths
  for u16 index upload with primitive restart, measured in DeS (Boletaria):
  SSE4.1 0.64%, AVX2 0.59%, AVX-512 0.56%. We already have an SSE strip path;
  the gain is small.
- GETLLAR and usleep work: PRs [#12523](https://github.com/RPCS3/rpcs3/pull/12523),
  [#12544](https://github.com/RPCS3/rpcs3/pull/12544),
  [#12605](https://github.com/RPCS3/rpcs3/pull/12605),
  [#15623](https://github.com/RPCS3/rpcs3/pull/15623) and
  [#16476](https://github.com/RPCS3/rpcs3/pull/16476). docs/28 already covers
  these ideas for our runtime.
- Facts worth keeping:
  - PR [#12587](https://github.com/RPCS3/rpcs3/pull/12587): DeS does not use
    raw SPUs.
  - #18470: "Demon's Souls isn't sensitive to FP accuracy" (Whatcookie).
  - PR [#8131](https://github.com/RPCS3/rpcs3/pull/8131): FMA with a 0 addend
    is common in DeS SPU code.
  - PR [#8694](https://github.com/RPCS3/rpcs3/pull/8694): such an FMA is not
    the same as a plain multiply, because of signed zero.
  - PR [#13690](https://github.com/RPCS3/rpcs3/pull/13690): 64-bit
    barycentrics in Vulkan; a comment reports DeS ran about 3x slower.
  - PR [#2660](https://github.com/RPCS3/rpcs3/pull/2660), in a comment: a
    lighting bug that makes the game dark, fixed by reloading the save. No
    detail.

## patch.yml and wiki notes

From a local copy of RPCS3's `patch.yml`. The DeS entries cover BLUS30443,
BLES00932, BCAS20071 (1.04), BCJS30022 (1.04) and the US trade demo
BLUD80018.

| Patch | Author | Note |
|---|---|---|
| Unlock FPS (v2.1) | Whatcookie, Gibbed | "Clocks scale and Vblank rate must be set to the defaults. Vblank rate can be modified to increase the framelimit beyond 60 FPS. However, it's not recommended as users may encounter physics issues." |
| Skip Intro Videos | GalCiv | Two `be32` writes at 0x008eef78 (US). |
| Aspect Ratio | Whatcookie | Needs "Stretch To Display Area"; UI is stretched. |
| Aspect ratio (HUD/Menus) 32:9 and 21:9 | Pierre | Offsets the UI quad to centre the HUD (US only). |
| Disable Motion Blur | illusion | |
| Enable FreeCam | horkrux | X + L3 cycles modes. |
| (note only) | | "Can disable Write Color Buffers for 20% faster performance ... slightly darker tone." This note is defined but not attached to any patch in this copy. |

Notes for the port:
- The FPS patch note warns of physics issues above 60 fps. This matches our
  open question about DES_FPS above 60.
- From our earlier notes: the RPCS3 FPS patch measures dt in the frame update
  (a code cave at 0x1B964), while the port measures it at flip.
- Wiki: not reachable (Cloudflare). Issue
  [#16157](https://github.com/RPCS3/rpcs3/issues/16157) says the wiki then
  recommended "SPURS Threads = 3" for DeS, and that setting hung the game on a
  loading screen until PR #16182. That is an emulator setting with no
  equivalent in the port.

## Does not apply

One line per issue. The "Why not" column is my classification from the title,
body and comments.

| Issue | State | Title | Why not |
|---|---|---|---|
| [#1486](https://github.com/RPCS3/rpcs3/issues/1486) | closed | Exception: Access violation reading location 0x0000000c | RPCS3 internals (JIT, LLVM, scheduler, settings, host CPU feature) |
| [#1870](https://github.com/RPCS3/rpcs3/issues/1870) | closed | PPU: LLVM recompiler issue | RPCS3 internals (JIT, LLVM, scheduler, settings, host CPU feature) |
| [#2359](https://github.com/RPCS3/rpcs3/issues/2359) | closed | Several games fails on Invalid RSX method | RPCS3 regression fixed inside RPCS3 code; no PS3-side lesson found |
| [#2516](https://github.com/RPCS3/rpcs3/issues/2516) | closed | Cannot launch games on Linux build (F LDR: St11range_error thrown: Narrow error | host platform or packaging (Linux, macOS, arm64, Steam Deck) |
| [#2538](https://github.com/RPCS3/rpcs3/issues/2538) | closed | Demon's Souls loading error | PSN copy, not mountable then |
| [#2621](https://github.com/RPCS3/rpcs3/issues/2621) | closed | Demon's Souls Black Screen | support, user setup, bad dump or stale; no fix |
| [#2674](https://github.com/RPCS3/rpcs3/issues/2674) | closed | Demon's Souls [BLES00932] freezes when using LLVM Recompiler. | RPCS3 internals (JIT, LLVM, scheduler, settings, host CPU feature) |
| [#2707](https://github.com/RPCS3/rpcs3/issues/2707) | closed | GL/VK: Tales of Vesperia - Rendering issues | different game |
| [#2735](https://github.com/RPCS3/rpcs3/issues/2735) | closed | N2gl4glsl14link_exceptionE thrown: linkage failed: 'error: fragment shader in... | host GPU driver or RPCS3 Vulkan/GL/D3D12 backend |
| [#2755](https://github.com/RPCS3/rpcs3/issues/2755) | closed | GL: Demon Souls - Garbage Graphics | GL clip outputs; PR #2773 |
| [#2846](https://github.com/RPCS3/rpcs3/issues/2846) | closed | Demon Souls problem | support, user setup, bad dump or stale; no fix |
| [#2875](https://github.com/RPCS3/rpcs3/issues/2875) | closed | Qt instability problems | emulator UI, input mapping, savestates, pkg, logging, RPCN account |
| [#2917](https://github.com/RPCS3/rpcs3/issues/2917) | closed | [REGRESSION] FONTS differs between OpenGL and Vulkan renderers. | host GPU driver or RPCS3 Vulkan/GL/D3D12 backend |
| [#2974](https://github.com/RPCS3/rpcs3/issues/2974) | closed | F {rsx::thread} class std: ERROR  AFTER BATTLE INITIATION  | RPCS3 regression fixed inside RPCS3 code; no PS3-side lesson found |
| [#2998](https://github.com/RPCS3/rpcs3/issues/2998) | closed | Demon's Souls [BLES00932] - crash when trying to execute RSX debugger | emulator UI, input mapping, savestates, pkg, logging, RPCN account |
| [#3051](https://github.com/RPCS3/rpcs3/issues/3051) | closed | Demon's Souls [BLES00932] says: Working buffer not big enough | RPCS3 regression fixed inside RPCS3 code; no PS3-side lesson found |
| [#3073](https://github.com/RPCS3/rpcs3/issues/3073) | closed | Demon's Souls [BLES00932] - exception in Nvidia's nvoglv64.dll | host GPU driver or RPCS3 Vulkan/GL/D3D12 backend |
| [#3074](https://github.com/RPCS3/rpcs3/issues/3074) | closed | Demon's Souls [BLES00932] says: Unknown comparison function 0x37c in file \RS... | "generic cell synchronization bug" per kd-11 |
| [#3116](https://github.com/RPCS3/rpcs3/issues/3116) | closed | Demons Souls Vulkan renderer freeze | host GPU driver or RPCS3 Vulkan/GL/D3D12 backend |
| [#3159](https://github.com/RPCS3/rpcs3/issues/3159) | closed | Couple issues with RCPS3 and Demon Souls. | support, user setup, bad dump or stale; no fix |
| [#3165](https://github.com/RPCS3/rpcs3/issues/3165) | closed | Regressions caused by zcull implementation (#3135) | RPCS3 regression fixed inside RPCS3 code; no PS3-side lesson found |
| [#3212](https://github.com/RPCS3/rpcs3/issues/3212) | closed | Fatal Error | support, user setup, bad dump or stale; no fix |
| [#3218](https://github.com/RPCS3/rpcs3/issues/3218) | closed | [BLUS30443] Demon's Souls crash during/after cutscenes | host platform or packaging (Linux, macOS, arm64, Steam Deck) |
| [#3266](https://github.com/RPCS3/rpcs3/issues/3266) | closed | [Arch Linux] [BLUS30443] Demon's Souls randomly crashes with Vulkan and LLVM | host platform or packaging (Linux, macOS, arm64, Steam Deck) |
| [#3274](https://github.com/RPCS3/rpcs3/issues/3274) | closed | Command line launching results in crash | emulator UI, input mapping, savestates, pkg, logging, RPCN account |
| [#3305](https://github.com/RPCS3/rpcs3/issues/3305) | closed | Demons Souls random crashing | support, user setup, bad dump or stale; no fix |
| [#3456](https://github.com/RPCS3/rpcs3/issues/3456) | closed | Demon's Souls [BLUS30443]: Vanguard boss not rendering | bad dump |
| [#3564](https://github.com/RPCS3/rpcs3/issues/3564) | closed | Demon's Souls- BLES00932 Hangs on Loading Screen | support, user setup, bad dump or stale; no fix |
| [#3577](https://github.com/RPCS3/rpcs3/issues/3577) | closed | Demon's Souls [BLES00932] random crash | support, user setup, bad dump or stale; no fix |
| [#3612](https://github.com/RPCS3/rpcs3/issues/3612) | closed | Xbox One Controller for Windows (USB): Select Button doesn't work | emulator UI, input mapping, savestates, pkg, logging, RPCN account |
| [#3613](https://github.com/RPCS3/rpcs3/issues/3613) | closed | Vulkan API call failed with unrecoverable error: Device lost | host GPU driver or RPCS3 Vulkan/GL/D3D12 backend |
| [#3626](https://github.com/RPCS3/rpcs3/issues/3626) | closed | Thread (DLSpursManagerCellSpursKernel1)} class std::runtime_error thrown: Unk... | closed without a fix. Rare SPU "Unknown STOP code 0x2a80" on save load. Watch list |
| [#3677](https://github.com/RPCS3/rpcs3/issues/3677) | closed | Demon's Souls Setup Issues | support, user setup, bad dump or stale; no fix |
| [#3792](https://github.com/RPCS3/rpcs3/issues/3792) | closed | [BLES00932] [Linux] [AMD] Vulkan/OpenGL primitive restart hang | driver hang on primitive restart; PR #4505 |
| [#4092](https://github.com/RPCS3/rpcs3/issues/4092) | closed | Demon's Souls Memory access violation and crashes after loss of sound and som... | support, user setup, bad dump or stale; no fix |
| [#4130](https://github.com/RPCS3/rpcs3/issues/4130) | closed | Regression: #3858 causes slowdown on Windows 10 + Vulkan. | RPCS3 regression fixed inside RPCS3 code; no PS3-side lesson found |
| [#4165](https://github.com/RPCS3/rpcs3/issues/4165) | closed | LLVM - Assertion failed Expression: !HasError && "Cannot get value when an er... | RPCS3 internals (JIT, LLVM, scheduler, settings, host CPU feature) |
| [#4195](https://github.com/RPCS3/rpcs3/issues/4195) | closed | Vulkan Crashing on new AMD Adrenalin drivers | host GPU driver or RPCS3 Vulkan/GL/D3D12 backend |
| [#4298](https://github.com/RPCS3/rpcs3/issues/4298) | closed | Persona 5: Random font/texture swapping | different game |
| [#4433](https://github.com/RPCS3/rpcs3/issues/4433) | closed | 3D Dot Game Heroes random freezing | different game |
| [#4434](https://github.com/RPCS3/rpcs3/issues/4434) | closed | RPCS3 Game Settings Database | emulator UI, input mapping, savestates, pkg, logging, RPCN account |
| [#4494](https://github.com/RPCS3/rpcs3/issues/4494) | closed | After installing the patch to run the game and then closing the game, it can ... | support, user setup, bad dump or stale; no fix |
| [#4555](https://github.com/RPCS3/rpcs3/issues/4555) | closed | Regression: Verification failed (e=0xb7) on PR #4554 | RPCS3 regression fixed inside RPCS3 code; no PS3-side lesson found |
| [#4568](https://github.com/RPCS3/rpcs3/issues/4568) | closed | Demon's Souls reliably hangs after build 6718 | RPCS3 regression fixed inside RPCS3 code; no PS3-side lesson found |
| [#4779](https://github.com/RPCS3/rpcs3/issues/4779) | closed | Regression: Nier [BLJM60223] Too dark lighting | different game |
| [#4887](https://github.com/RPCS3/rpcs3/issues/4887) | closed | Demon's Souls hangs with verification problem error | RPCS3 regression fixed inside RPCS3 code; no PS3-side lesson found |
| [#4929](https://github.com/RPCS3/rpcs3/issues/4929) | closed | Unhandled Win32 exception on Silent Hill: Downpour | different game |
| [#5019](https://github.com/RPCS3/rpcs3/issues/5019) | closed | Demon Souls freezes with Verification failed | RPCS3 regression fixed inside RPCS3 code; no PS3-side lesson found |
| [#5025](https://github.com/RPCS3/rpcs3/issues/5025) | closed | RADV graphical glitches on Demon Souls | host GPU driver or RPCS3 Vulkan/GL/D3D12 backend |
| [#5037](https://github.com/RPCS3/rpcs3/issues/5037) | closed | Demon Souls freezes after 15-25 minutes of play | support, user setup, bad dump or stale; no fix |
| [#5059](https://github.com/RPCS3/rpcs3/issues/5059) | closed | Demon Souls (BLES00932) Visual Bugs | support, user setup, bad dump or stale; no fix |
| [#5141](https://github.com/RPCS3/rpcs3/issues/5141) | closed | Demon's Souls [BLES00932] - Stuck on loading screen | support, user setup, bad dump or stale; no fix |
| [#5171](https://github.com/RPCS3/rpcs3/issues/5171) | closed | Demon's Souls - [BLUS30443] crashes or freezes RPCS3 in first minute | support, user setup, bad dump or stale; no fix |
| [#5192](https://github.com/RPCS3/rpcs3/issues/5192) | closed | Feature request: Possibility to map, one or more key to a single button | emulator UI, input mapping, savestates, pkg, logging, RPCN account |
| [#5228](https://github.com/RPCS3/rpcs3/issues/5228) | closed | Crash with ***LINUX*** when entering loading screen in Demon's Souls [BLES00932] | host platform or packaging (Linux, macOS, arm64, Steam Deck) |
| [#5355](https://github.com/RPCS3/rpcs3/issues/5355) | closed | BLES00932 - Demon's Souls RPCS3 Crash during cutscenes | RPCS3 regression fixed inside RPCS3 code; no PS3-side lesson found |
| [#5594](https://github.com/RPCS3/rpcs3/issues/5594) | closed | Strange Vulkan framepacing issue in Persona 5 | different game |
| [#5831](https://github.com/RPCS3/rpcs3/issues/5831) | open | PPU: Games that still don't work with LLVM recompiler | open; no fix to copy |
| [#5921](https://github.com/RPCS3/rpcs3/issues/5921) | closed | Demon's Souls regressed on OpenGL after #5895 | host GPU driver or RPCS3 Vulkan/GL/D3D12 backend |
| [#6226](https://github.com/RPCS3/rpcs3/issues/6226) | closed | RPCS3 stopped starting when installed games are on windows share - regression | emulator UI, input mapping, savestates, pkg, logging, RPCN account |
| [#6287](https://github.com/RPCS3/rpcs3/issues/6287) | closed | Demon's Souls [BLES00932] flickering on AMD Vega | host GPU driver or RPCS3 Vulkan/GL/D3D12 backend |
| [#6327](https://github.com/RPCS3/rpcs3/issues/6327) | closed | Demon's Souls [BLUS30443] crashing | support, user setup, bad dump or stale; no fix |
| [#6343](https://github.com/RPCS3/rpcs3/issues/6343) | closed | Audio crackling after ~5 minutes of emulation | host audio backend or buffering |
| [#6370](https://github.com/RPCS3/rpcs3/issues/6370) | closed | God Of war 2 hd.....blue and red interchange for vulkan.....and Open Gl :-can... | different game |
| [#6518](https://github.com/RPCS3/rpcs3/issues/6518) | closed | On-screen keyboard does not obey enter button assignment setting | emulator UI, input mapping, savestates, pkg, logging, RPCN account |
| [#6618](https://github.com/RPCS3/rpcs3/issues/6618) | closed | Demon's Souls [BLES00932] issue with async shaders on AMD Navi GPU | host GPU driver or RPCS3 Vulkan/GL/D3D12 backend |
| [#6627](https://github.com/RPCS3/rpcs3/issues/6627) | closed | Demon's Souls [BLES00932] softlocks on i7-8700 CPUs with TSX enabled | TSX-only softlock with music on. Looks like our fixed lwarx freeze (guess) |
| [#6632](https://github.com/RPCS3/rpcs3/issues/6632) | closed | Demon's Souls [BLES00932] nv406e::semaphore_acquire has timed out. | missing RSX init between runs; PR #6654 |
| [#6876](https://github.com/RPCS3/rpcs3/issues/6876) | closed | Cannot apply patches using patch.yml | emulator UI, input mapping, savestates, pkg, logging, RPCN account |
| [#7035](https://github.com/RPCS3/rpcs3/issues/7035) | closed | Demon Souls: after a long play renderer(vulkan) starts freezing. Stably. | host GPU driver or RPCS3 Vulkan/GL/D3D12 backend |
| [#7502](https://github.com/RPCS3/rpcs3/issues/7502) | closed | [NVIDIA ONLY] vulkan performance regression | host GPU driver or RPCS3 Vulkan/GL/D3D12 backend |
| [#7713](https://github.com/RPCS3/rpcs3/issues/7713) | closed | Demon's Souls [BLES00932] regression on OpenGL | host GPU driver or RPCS3 Vulkan/GL/D3D12 backend |
| [#8133](https://github.com/RPCS3/rpcs3/issues/8133) | closed | Demon's Souls [BLUS30443] :- Performance regression from 0.0.9-10279 #8051 on... | RPCS3 regression fixed inside RPCS3 code; no PS3-side lesson found |
| [#8344](https://github.com/RPCS3/rpcs3/issues/8344) | closed | Problem starting demon souls | support, user setup, bad dump or stale; no fix |
| [#8363](https://github.com/RPCS3/rpcs3/issues/8363) | closed | Games running way better with their windows' unfocused | RPCS3 internals (JIT, LLVM, scheduler, settings, host CPU feature) |
| [#8791](https://github.com/RPCS3/rpcs3/issues/8791) | closed | Demon's Souls - no sound after boot screen. | user had sound off in game options. Log shows the expected EDEADLK |
| [#8955](https://github.com/RPCS3/rpcs3/issues/8955) | closed | Vulkan Crash on 0.0.12-10865 till 0.0.12-10915 work fine on 0.0.12-10811 & 0.... | host GPU driver or RPCS3 Vulkan/GL/D3D12 backend |
| [#9082](https://github.com/RPCS3/rpcs3/issues/9082) | closed | Demon's Souls does not function properly with mods on NA/EU versions | support, user setup, bad dump or stale; no fix |
| [#9144](https://github.com/RPCS3/rpcs3/issues/9144) | closed | All game freeze after 5-10 minutes | support, user setup, bad dump or stale; no fix |
| [#9152](https://github.com/RPCS3/rpcs3/issues/9152) | closed | [Feature Request] Detect and inform users about Vulkan issues | emulator UI, input mapping, savestates, pkg, logging, RPCN account |
| [#9238](https://github.com/RPCS3/rpcs3/issues/9238) | closed | Games freezing with TSX-FA after (#9232) | RPCS3 internals (JIT, LLVM, scheduler, settings, host CPU feature) |
| [#9250](https://github.com/RPCS3/rpcs3/issues/9250) | closed | Demons Souls BLES00932 random crashes  | support, user setup, bad dump or stale; no fix |
| [#9358](https://github.com/RPCS3/rpcs3/issues/9358) | closed | Minor FPS regression in Demon's Souls after #9241 | RPCS3 regression fixed inside RPCS3 code; no PS3-side lesson found |
| [#9403](https://github.com/RPCS3/rpcs3/issues/9403) | closed | New regression on Demons souls build 11364 stuck on black screen after initia... | RPCS3 regression fixed inside RPCS3 code; no PS3-side lesson found |
| [#9442](https://github.com/RPCS3/rpcs3/issues/9442) | closed | Dark Souls 2 freezing repeatedly during gameplay | different game |
| [#9580](https://github.com/RPCS3/rpcs3/issues/9580) | open | [Feature Request] Support PFD protected save data | open; no fix to copy |
| [#9651](https://github.com/RPCS3/rpcs3/issues/9651) | closed | [Feature Request] Online Support For NPUA80139 | different game |
| [#9704](https://github.com/RPCS3/rpcs3/issues/9704) | closed | Demon's Souls [BLES00932] segfaults on launch since 58eef983e23 | RPCS3 regression fixed inside RPCS3 code; no PS3-side lesson found |
| [#9718](https://github.com/RPCS3/rpcs3/issues/9718) | closed | Demon's Souls [BLES00932] flickering rendering issues in the Tower of Latria ... | AMD driver only |
| [#9856](https://github.com/RPCS3/rpcs3/issues/9856) | closed | Demon's Souls Stuttering every few seconds | dup of #9857 |
| [#9858](https://github.com/RPCS3/rpcs3/issues/9858) | closed | Demon's Souls [BLUS30443] Stuttering | dup of #9857 |
| [#10100](https://github.com/RPCS3/rpcs3/issues/10100) | closed | On-Screen Keyboard (OSK) unable to output blank fields | emulator UI, input mapping, savestates, pkg, logging, RPCN account |
| [#10137](https://github.com/RPCS3/rpcs3/issues/10137) | closed | Demons Souls BLUS30443 Has Stopped Working After Power Outage... | support, user setup, bad dump or stale; no fix |
| [#11003](https://github.com/RPCS3/rpcs3/issues/11003) | closed | Issue with unlocking Soldier's Trophy on Demon's Souls [BLUS30443] | support, user setup, bad dump or stale; no fix |
| [#11052](https://github.com/RPCS3/rpcs3/issues/11052) | closed | Sort screenshots into different folders by game | emulator UI, input mapping, savestates, pkg, logging, RPCN account |
| [#11154](https://github.com/RPCS3/rpcs3/issues/11154) | closed | Demon's Souls stuck on loading screen | support, user setup, bad dump or stale; no fix |
| [#11177](https://github.com/RPCS3/rpcs3/issues/11177) | closed | Performance regression on some CPU's | RPCS3 regression fixed inside RPCS3 code; no PS3-side lesson found |
| [#11244](https://github.com/RPCS3/rpcs3/issues/11244) | closed | Demon's Souls slow loading screen + Suppress all logs | emulator UI, input mapping, savestates, pkg, logging, RPCN account |
| [#11610](https://github.com/RPCS3/rpcs3/issues/11610) | closed | I cant open ANY game | support, user setup, bad dump or stale; no fix |
| [#11684](https://github.com/RPCS3/rpcs3/issues/11684) | closed | Demon's Souls [BLES00932] crashing after character creation on Steam Deck | host platform or packaging (Linux, macOS, arm64, Steam Deck) |
| [#11703](https://github.com/RPCS3/rpcs3/issues/11703) | closed | Regression:- Demon's Souls [BLUS30443] freezes after building SPU cache upon ... | not reproducible; the related DeS crash fix PR #11701 is listed above |
| [#11901](https://github.com/RPCS3/rpcs3/issues/11901) | closed | [Trace inside] Crash when going ingame in Demon's Souls (BLUS30443) | RPCS3 regression fixed inside RPCS3 code; no PS3-side lesson found |
| [#12123](https://github.com/RPCS3/rpcs3/issues/12123) | closed | Sleeping and Waking the Steam Deck Freezes Emulation | host platform or packaging (Linux, macOS, arm64, Steam Deck) |
| [#12311](https://github.com/RPCS3/rpcs3/issues/12311) | closed | Significant performance regression with noticeably increased CPU utilization ... | RPCS3 regression fixed inside RPCS3 code; no PS3-side lesson found |
| [#12429](https://github.com/RPCS3/rpcs3/issues/12429) | closed | RPCS3 stops responding when launching games after a short while of the PC run... | emulator UI, input mapping, savestates, pkg, logging, RPCN account |
| [#12513](https://github.com/RPCS3/rpcs3/issues/12513) | closed | Trying to load a Demon's Souls savestate with the UNLOCK FPS patch enabled cr... | emulator UI, input mapping, savestates, pkg, logging, RPCN account |
| [#12565](https://github.com/RPCS3/rpcs3/issues/12565) | closed | Regression: Dark Souls (BLES01765) crashes at start after PR (#12523) | Dark Souls, LLVM |
| [#12718](https://github.com/RPCS3/rpcs3/issues/12718) | closed | Cannot connect to RPCN to create an account | emulator UI, input mapping, savestates, pkg, logging, RPCN account |
| [#12723](https://github.com/RPCS3/rpcs3/issues/12723) | closed | Demon's Souls graphical  | support, user setup, bad dump or stale; no fix |
| [#12882](https://github.com/RPCS3/rpcs3/issues/12882) | closed | [macOS] [Arm Native] LLVM: Segfault before launching any game  | host platform or packaging (Linux, macOS, arm64, Steam Deck) |
| [#13087](https://github.com/RPCS3/rpcs3/issues/13087) | closed | [Regression] Demon Souls black screen | RPCS3 regression fixed inside RPCS3 code; no PS3-side lesson found |
| [#13171](https://github.com/RPCS3/rpcs3/issues/13171) | closed | AppImage doesn't run on Wayland | host platform or packaging (Linux, macOS, arm64, Steam Deck) |
| [#13310](https://github.com/RPCS3/rpcs3/issues/13310) | closed | [Regression] Audio crackling on Linux when audio buffering is enabled (#11358)  | host audio backend or buffering |
| [#13376](https://github.com/RPCS3/rpcs3/issues/13376) | closed | [Bug] "Resolution Scale" setting affecting "Default Resolution" setting at 1080p | emulator UI, input mapping, savestates, pkg, logging, RPCN account |
| [#13520](https://github.com/RPCS3/rpcs3/issues/13520) | closed | Sound problem in Dark Souls II: Scholar of the First Sin [BLES02135] on MAC | different game |
| [#13589](https://github.com/RPCS3/rpcs3/issues/13589) | closed | [Regression] Minor performance regression with increased CPU utilization in m... | RPCS3 regression fixed inside RPCS3 code; no PS3-side lesson found |
| [#13638](https://github.com/RPCS3/rpcs3/issues/13638) | closed | [Regression] Perfomance drop after LLVM 16.0.0 update (#13534) | RPCS3 internals (JIT, LLVM, scheduler, settings, host CPU feature) |
| [#13868](https://github.com/RPCS3/rpcs3/issues/13868) | closed | Opengl initialization failure | host GPU driver or RPCS3 Vulkan/GL/D3D12 backend |
| [#13936](https://github.com/RPCS3/rpcs3/issues/13936) | closed | Demon's Souls fails to load in nonstandard EmulationDir | emulator UI, input mapping, savestates, pkg, logging, RPCN account |
| [#14036](https://github.com/RPCS3/rpcs3/issues/14036) | closed | Launcher freeze when starting a game | emulator UI, input mapping, savestates, pkg, logging, RPCN account |
| [#14161](https://github.com/RPCS3/rpcs3/issues/14161) | closed | [Profiling] Slow pkg installation due to ecdsa_verify/check_ecdsa | emulator UI, input mapping, savestates, pkg, logging, RPCN account |
| [#14200](https://github.com/RPCS3/rpcs3/issues/14200) | closed | Demon souls vulkan crash | host GPU driver or RPCS3 Vulkan/GL/D3D12 backend |
| [#14439](https://github.com/RPCS3/rpcs3/issues/14439) | closed | [Regression] Multiple PPU related issues since 0.0.29-15457 (#14437) | RPCS3 internals (JIT, LLVM, scheduler, settings, host CPU feature) |
| [#14510](https://github.com/RPCS3/rpcs3/issues/14510) | closed | AUDIT fails in on_ranged_block_first_section_created | RPCS3 regression fixed inside RPCS3 code; no PS3-side lesson found |
| [#14699](https://github.com/RPCS3/rpcs3/issues/14699) | closed | [Regression] SPU Cache stuck  | RPCS3 internals (JIT, LLVM, scheduler, settings, host CPU feature) |
| [#14700](https://github.com/RPCS3/rpcs3/issues/14700) | closed | [Regression] Demon's Souls | RPCS3 regression fixed inside RPCS3 code; no PS3-side lesson found |
| [#14780](https://github.com/RPCS3/rpcs3/issues/14780) | closed | No sound in game/menus but intro sound is ok | host audio backend or buffering |
| [#14796](https://github.com/RPCS3/rpcs3/issues/14796) | closed | Black Screen on Demon Souls | support, user setup, bad dump or stale; no fix |
| [#15092](https://github.com/RPCS3/rpcs3/issues/15092) | open | macOS: Crash on game savedata dialogs | host platform or packaging (Linux, macOS, arm64, Steam Deck) |
| [#15193](https://github.com/RPCS3/rpcs3/issues/15193) | closed | [Regression] Massive performance degradation (#15175) | RPCS3 regression fixed inside RPCS3 code; no PS3-side lesson found |
| [#15194](https://github.com/RPCS3/rpcs3/issues/15194) | closed | Demon's souls | support, user setup, bad dump or stale; no fix |
| [#15222](https://github.com/RPCS3/rpcs3/issues/15222) | closed | Demon’s Souls crashes during boot on Aarch64 Linux | host platform or packaging (Linux, macOS, arm64, Steam Deck) |
| [#15228](https://github.com/RPCS3/rpcs3/issues/15228) | closed | Shortcut issues macOS | host platform or packaging (Linux, macOS, arm64, Steam Deck) |
| [#15231](https://github.com/RPCS3/rpcs3/issues/15231) | closed | Savestates are not being detected that are form a different rpcs3  | emulator UI, input mapping, savestates, pkg, logging, RPCN account |
| [#15241](https://github.com/RPCS3/rpcs3/issues/15241) | closed | Demons souls crashing after beating a boss | support, user setup, bad dump or stale; no fix |
| [#15517](https://github.com/RPCS3/rpcs3/issues/15517) | closed | Virtua Tennis 4 - case lists max res as 1080i but RPCS3 "Supported Resolution... | different game |
| [#15631](https://github.com/RPCS3/rpcs3/issues/15631) | closed | macOS Shortcut creation error: unable to add icon to bundle (probable easy fix) | host platform or packaging (Linux, macOS, arm64, Steam Deck) |
| [#15724](https://github.com/RPCS3/rpcs3/issues/15724) | closed | Demon's Souls [BLES00932] - Broken sound  | support, user setup, bad dump or stale; no fix |
| [#15819](https://github.com/RPCS3/rpcs3/issues/15819) | open | [Regression] Mouse in fullscreen not working properly | emulator UI, input mapping, savestates, pkg, logging, RPCN account |
| [#15827](https://github.com/RPCS3/rpcs3/issues/15827) | closed | consistent popping sound when audio buffering is on in demon's souls | host audio backend or buffering |
| [#15947](https://github.com/RPCS3/rpcs3/issues/15947) | closed | "HORI. Real Arcade Pro.V HS 1" stops working with HIDAPI error after stop gam... | emulator UI, input mapping, savestates, pkg, logging, RPCN account |
| [#15970](https://github.com/RPCS3/rpcs3/issues/15970) | closed | [Regression] arm64: Many games stuck on early loading stage (#15962) | host platform or packaging (Linux, macOS, arm64, Steam Deck) |
| [#15986](https://github.com/RPCS3/rpcs3/issues/15986) | closed | gl/v3d: Several applications and games require shader cache to be removed bef... | host GPU driver or RPCS3 Vulkan/GL/D3D12 backend |
| [#16002](https://github.com/RPCS3/rpcs3/issues/16002) | closed | [Bug] [macOS] [Arm] LLVM compiler crashes on complicated games | host platform or packaging (Linux, macOS, arm64, Steam Deck) |
| [#16027](https://github.com/RPCS3/rpcs3/issues/16027) | closed | Demon Souls shows black screen in Ubuntu 22.04 and RX Vega 56 | host GPU driver or RPCS3 Vulkan/GL/D3D12 backend |
| [#16155](https://github.com/RPCS3/rpcs3/issues/16155) | closed | [Mega Issue] Tracker of "Max SPURS Threads" regressions | RPCS3 internals (JIT, LLVM, scheduler, settings, host CPU feature) |
| [#16157](https://github.com/RPCS3/rpcs3/issues/16157) | closed | [Documentation] Demon's Souls [BLUS30443] - Wiki recommends SPURS Threads = 3... | wiki advised SPURS Threads = 3; fixed by PR #16182 |
| [#16216](https://github.com/RPCS3/rpcs3/issues/16216) | closed | [Mega Issue] Savestates | emulator UI, input mapping, savestates, pkg, logging, RPCN account |
| [#16242](https://github.com/RPCS3/rpcs3/issues/16242) | closed | Some of the audio of Demon's Souls seems to rapidly repeat at seemingly rando... | kd-11: the 3-core FX-6300 is the culprit |
| [#16246](https://github.com/RPCS3/rpcs3/issues/16246) | closed | Crash in Demon's Souls  | support, user setup, bad dump or stale; no fix |
| [#16349](https://github.com/RPCS3/rpcs3/issues/16349) | closed | BCES00609: InFamous - Vulkan Error "Device Lost" | different game |
| [#16382](https://github.com/RPCS3/rpcs3/issues/16382) | closed | [Regression] Performance regression since v0.0.33-17061-af052b06 | RPCS3 regression fixed inside RPCS3 code; no PS3-side lesson found |
| [#16520](https://github.com/RPCS3/rpcs3/issues/16520) | closed | Spelunker HD NPUB30152 music tempo bug | different game |
| [#16622](https://github.com/RPCS3/rpcs3/issues/16622) | closed | [macOS] RPCS3 saves some caches in wrong folder, does not delete completely | host platform or packaging (Linux, macOS, arm64, Steam Deck) |
| [#16833](https://github.com/RPCS3/rpcs3/issues/16833) | closed | White Knight Chronicles II Character Movement issue | different game |
| [#16897](https://github.com/RPCS3/rpcs3/issues/16897) | open | [Feature request] Improved KBM handling (mouse-to-stick translation) | emulator UI, input mapping, savestates, pkg, logging, RPCN account |
| [#16909](https://github.com/RPCS3/rpcs3/issues/16909) | open | [Regression] Heavy stutter in God of War 3 [BCES00510] from build 0.0.28-1540... | different game |
| [#17167](https://github.com/RPCS3/rpcs3/issues/17167) | open | [BLUS30443] Demon's Souls - Strange Echoing Audio | open; no fix to copy |
| [#17291](https://github.com/RPCS3/rpcs3/issues/17291) | open | Low CPU and GPU utilization in Demon's Souls [BLES00932] | open; no fix to copy |
| [#17326](https://github.com/RPCS3/rpcs3/issues/17326) | closed | [Regression] Visual Studio 17.14 introduces a compiler regression causing no ... | MSVC 17.14 codegen; no audio with HLE libadec |
| [#17328](https://github.com/RPCS3/rpcs3/issues/17328) | closed | [Regression] Tekken Tag Tournament 2: No Background Audio | different game |
| [#17395](https://github.com/RPCS3/rpcs3/issues/17395) | closed | Demon's souls. Not background music in menu | dup of #17326 |
| [#17530](https://github.com/RPCS3/rpcs3/issues/17530) | closed | [Regression] [macOS] [Arm] Games fail to launch | host platform or packaging (Linux, macOS, arm64, Steam Deck) |
| [#17542](https://github.com/RPCS3/rpcs3/issues/17542) | open | WolfSSL error when trying to register on RPCN | emulator UI, input mapping, savestates, pkg, logging, RPCN account |
| [#17651](https://github.com/RPCS3/rpcs3/issues/17651) | closed | RPCS3 freeze when trying to savestate | emulator UI, input mapping, savestates, pkg, logging, RPCN account |
| [#17659](https://github.com/RPCS3/rpcs3/issues/17659) | open | Still crashes when running any game on arm macOS | host platform or packaging (Linux, macOS, arm64, Steam Deck) |
| [#17885](https://github.com/RPCS3/rpcs3/issues/17885) | closed | [Regression] Demon's Souls black screen (#17874) | RPCS3 regression fixed inside RPCS3 code; no PS3-side lesson found |
| [#17913](https://github.com/RPCS3/rpcs3/issues/17913) | closed | [Regression] [macOS] Graphical issues in several games | host platform or packaging (Linux, macOS, arm64, Steam Deck) |
| [#17991](https://github.com/RPCS3/rpcs3/issues/17991) | closed | Demon's Souls crashes at Latrian archstone | support, user setup, bad dump or stale; no fix |
| [#17997](https://github.com/RPCS3/rpcs3/issues/17997) | open | [Feature request] loader: CHD boot support | emulator UI, input mapping, savestates, pkg, logging, RPCN account |
| [#18087](https://github.com/RPCS3/rpcs3/issues/18087) | open | Demon's souls struggles in big areas | open; no fix to copy |
| [#18094](https://github.com/RPCS3/rpcs3/issues/18094) | open | Spider-Man crashes a few minutes into gameplay with ERROR_NO_SYSTEM_RESOURCES... | different game |
| [#18156](https://github.com/RPCS3/rpcs3/issues/18156) | closed | '#define' : Macro redefined; different substitutions: TEX_PARAM | RPCS3 internals (JIT, LLVM, scheduler, settings, host CPU feature) |
| [#18334](https://github.com/RPCS3/rpcs3/issues/18334) | open | AMD Radeon 720p Misalignment Diagonal Visual Bug | host GPU driver or RPCS3 Vulkan/GL/D3D12 backend |
| [#18470](https://github.com/RPCS3/rpcs3/issues/18470) | closed | [Regression][macOS] Several games not working on Arm after PR (#18395) | host platform or packaging (Linux, macOS, arm64, Steam Deck) |
| [#18530](https://github.com/RPCS3/rpcs3/issues/18530) | open | Crash when loading firmware or any game on MacBook Pro m3 pro, macOS 26.4+ | host platform or packaging (Linux, macOS, arm64, Steam Deck) |
| [#18569](https://github.com/RPCS3/rpcs3/issues/18569) | open | [BLUS30782] Dark Souls - Frame pacing instability | different game |
| [#18600](https://github.com/RPCS3/rpcs3/issues/18600) | closed | Worse GPU Performance on Pascal after vulkan rewrites | host GPU driver or RPCS3 Vulkan/GL/D3D12 backend |
| [#18622](https://github.com/RPCS3/rpcs3/issues/18622) | closed | Incorrect color and depth rendering in some From Software games in AMD Vulkan... | host GPU driver or RPCS3 Vulkan/GL/D3D12 backend |
| [#18684](https://github.com/RPCS3/rpcs3/issues/18684) | open | [NPUB31347] ACE COMBAT Infinity Graphics Glitches | different game |
| [#18698](https://github.com/RPCS3/rpcs3/issues/18698) | closed | Dark Souls 2 gets stuck with installing game files. | different game |
| [#18699](https://github.com/RPCS3/rpcs3/issues/18699) | open | [Regression] Emulator crashes when compiling PPU | RPCS3 internals (JIT, LLVM, scheduler, settings, host CPU feature) |
| [#18950](https://github.com/RPCS3/rpcs3/issues/18950) | closed | [Regression] Devil May Cry HD Collection crashes before Capcom logo with Vulk... | different game |
| [#18974](https://github.com/RPCS3/rpcs3/issues/18974) | closed | [Regression] Demon's Souls crashing after #18395 | RPCS3 regression fixed inside RPCS3 code; no PS3-side lesson found |
| [#19433](https://github.com/RPCS3/rpcs3/issues/19433) | closed | [Regression] Shader Quality: Low broken after #19410 (Rewrite how blending wo... | RPCS3 regression fixed inside RPCS3 code; no PS3-side lesson found |
| [#19505](https://github.com/RPCS3/rpcs3/issues/19505) | closed | Demon's Souls crashes on latest version | support, user setup, bad dump or stale; no fix |
| [#19804](https://github.com/RPCS3/rpcs3/issues/19804) | open | [BCUS98152] SOCOM: Confrontation - Black Flickering Issue | different game |
