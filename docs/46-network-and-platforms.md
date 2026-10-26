# Online play, and Linux/macOS additions (survey, 2026-10-18)

A docs-only survey. Nothing was built or run, and no game or community server was contacted. Addresses are for BLUS30443 01.00 (`work/game/EBOOT.ELF`). A virtual address (VA) is the file offset plus 0x10000 (`work/loader/EBOOT.image.json`). ps3recomp line numbers are from the fork's master at 8b77615. RPCS3 line numbers are from the reference checkout at 065b490.

Three labels are used below:
- **(read)**: I read it in source, in the ELF, in a log, or on the cited web page.
- **(search)**: from a web search summary. I did not open the page itself.
- **(inferred)**: my conclusion, not checked in a run. Section A8 lists the runs that would check these.

Part A covers online play. Part B adds to docs/36 (Linux and macOS) only where docs/36 was silent.

## Summary

### Online: feasible in stages, and the first useful stage is small

- **No TLS is needed.** The game talks to its server in plain HTTP through cellHttp. It imports no cellSsl, no cellHttps and no cellHttpInitSsl, and its URL template is `http://%s/%s` (read). The request bodies are AES-encrypted, but by the game's own code, which the lift already runs (read: DeSSE decrypts them server-side; inferred: no HLE crypto needed).
- **One host name needs redirecting for the US release.** It is `c.demons-souls.com:18000`, which serves `demons-souls-us/ss.info` (read: EBOOT strings). That file names the real game servers, so a private server points the client at itself (read: DeSSE).
- **No PSN identity check.** The game requests no NP ticket (no sceNpManagerGetTicket or RequestTicket import, read). The server only sees the name the client sends.
- **Co-op and PvP use NP Matching2 rooms plus P2P UDP.** The DeS server only relays the room ID between the two players (read: DeSSE).
- **ps3recomp already has most of the plumbing,** behind `PS3_NET_ONLINE` and a psnr server: rooms, signaling, NAT punching and a relay, verified with Simpsons Arcade. 97 of DeS's 128 network imports resolve today. The 31 that don't are mostly small (section A3):
  - 10 cellHttp functions, 4 of which exist under the wrong names;
  - 2 Base64 helpers;
  - 8 sceNp, 2 Matching2, 6 sys_net and 3 voice-chat functions.
- **Safety first.** Today cellHttp resolves host names through the OS whatever the online setting is (`cellHttp.c` line 498). Don't turn `PS3_NET_ONLINE` on to experiment until a host map that fails closed is in place. Otherwise the game would look up the original domains, which may now belong to someone else (inferred; not checked).

Recommended order:
1. **Stage 0, groundwork (4-5 days).** Add a Network section to the settings, Offline by default. Add a host map that fails closed. Let NP report signed in without a matching server. Add the small stubs.
2. **Stage 1, the shared world (4-7 days, plus 1-2 for a local test server).** This covers messages, bloodstains, wandering ghosts, death replays, world tendency and the server's message of the day.
   - Fix the cellHttp names and test against a local server.
   - Then ask the Archstones operator before pointing the port at it. On Archstones' RPCS3 server, port players would share messages and bloodstains with RPCS3 players (inferred).
3. **Stage 2, co-op and PvP among port players, through a psnr server (8-16 days).** Port players cannot join RPCS3 players' rooms, which live on RPCN. So summon signs need a separate pool (section A7).
4. **Stage 3, co-op with RPCS3 players (20-40 days, optional).** This needs a clean-room RPCN client plus RPCS3's P2P framing, and the RPCN maintainers' consent.

### Linux and macOS (additions to docs/36)

- **Proton/Wine:** every Windows piece the build imports has a Wine/Proton counterpart. Two real risks remain:
  - **Run-time shader compiles** through Wine's own d3dcompiler_47. Fix: ship Microsoft's d3dcompiler_47.dll next to the exe.
  - **The MPEG-2 movie decoder** in Media Foundation. Fallback: the FFmpeg path.
  - The menu's D3D11On12 needs DXVK 2.2 or newer, which Proton has (inferred, section B1).
- **Apple Silicon:** the PPU lift is portable C++. It has no SIMD intrinsics, and its barriers are C++ fences. So a native arm64 build looks possible (read, section B4).
  - Rosetta 2 runs only macOS x86-64 programs. So it saves the arm64 bring-up, but not the macOS port itself.
  - What Rosetta adds is x86 memory ordering. That keeps hiding the runtime's ordering bugs, as x86 does today (inferred).

### Effort and top risks

| Item | Days |
|---|---|
| A, Stage 0: settings, host map, NP signed in, stubs | 4-5 |
| A, Stage 1: cellHttp names, local test, Archstones | 4-7 (+1-2 local test server) |
| A, Stage 2: co-op and PvP among port players | 8-16 |
| A, Stage 3: RPCN client (optional) | 20-40 |
| Voice chat (not planned) | 10+ |
| B: Proton test and fixes | 2-4 (needs a Linux PC or Steam Deck) |
| B: CrossOver/D3DMetal test | 1-2 (needs a Mac; result unknown) |
| B: arm64 work beyond docs/36 | 4-7 |

Top risks:
- How the game behaves when NP is signed in but rooms are unavailable.
- Mixed port and RPCS3 populations on one DeS server.
- Untrusted network input reaching HLE code.
- Consent from the community server and RPCN operators.

Full list in A7.

## Part A. Online play

### A1. What DeS uses (read)

These come from `work/loader/EBOOT.imports.json` (389 imports). NIDs were matched to names through the port's NID table and the names RPCS3 registers.

| Module | Imports | What DeS uses them for |
|---|---|---|
| cellNetCtl | 8 | link state, IP info, the "connect to network" dialog |
| sceNp (manager, basic, score, util) | 34 | sign-in status and IDs, network time, friend and block lists, players met, rating flags; 13 of the 34 are sceNpScore calls |
| sceNp2 (Matching2) | 26 | contexts, server and world lists, create/join/search/leave room, room data, kick, grant owner, signaling status and callbacks |
| sys_net | 29 | BSD sockets, DNS, UDP P2P (`sys_net_set_udpp2p_test_param`), interface info |
| cellHttp | 18 | HTTP client, keep-alive pool, user agent |
| cellHttpUtil | 3 | URI parsing, Base64 |
| cellSysutilAvc2 | 10 | voice chat |

- **Call sites.** Each of these 128 imports has at least one call site in the lift (counted in `work/recomp`).
- **sceNpScore.** Whether the game ever reaches the 13 Score calls is unknown. DeS has no visible leaderboards (inferred).
- **Not imported:** cellSsl, cellHttps, cellHttpInitSsl, sceNpManagerGetTicket, sceNpManagerRequestTicket, the sceNpSignaling functions, and the old sceNpMatching (v1). So there is no TLS and no PSN ticket, and signaling runs only through Matching2.

Strings in the ELF (UTF-16BE):
- VA 0x016BC2C8: `demons-souls-us/ss.info`, followed by `c.demons-souls.com:18000`.
- VA 0x016E9CE8: `http://%s/%s`. VA 0x016E9D08: `%s%s.spd`, so the endpoints are named `<name>.spd`.
- No `g.demons-souls.com` or other region host appears in this release. The other names in community lists come from other regions, or from `ss.info` (inferred).

The game's network library runs on three threads, named `NexusRevolution Event`, `NexusRevolution Surveillance` and `NexusRevolution Main` (`work/tmp/play.txt`, lines 485-501, run of 2026-10-18 15:44).

What happens today (same log, lines 486-500):
1. cellNetCtl starts, and Matching2 reports "Init (offline)".
2. `cellNetCtlGetState` fails with NOT_INITIALIZED, which is ps3recomp's offline answer.
3. `sceNpManagerGetStatus` returns OFFLINE, and the manager callback fires OFFLINE.
4. Two NP imports are unresolved on this path: sceNpBasicGetMatchingInvitationEntryCount and sceNpManagerGetAvatarUrl.
5. No cellHttp call is made.

The title then shows "Cannot connect to the Demon's Souls server (009)" and offers Offline Mode (reported from play). What 009 means was not traced, and a web search found nothing on it.

### A2. How the service worked, and how it runs now

The official servers closed worldwide on 28 February 2018 (search: RPGamer).

#### Feature map

This table is read from DeSSE's handlers. The Matching2 column is partly inferred.

| Feature | DeS server (HTTP `.spd`) | NP Matching2 | P2P |
|---|---|---|---|
| Login, message of the day, player counts | `login` | - | - |
| Messages: read, write, rate, delete | `getBloodMessage`, `addBloodMessage`, `updateBloodMessageGrade`, `deleteBloodMessage`, `getBloodMessageGrade` | - | - |
| Bloodstains (death replays) | `getReplayList`, `getReplayData`, `addReplayData` | - | - |
| Wandering ghosts | `getWanderingGhost`, `setWanderingGhost` | - | - |
| World tendency | `getQWCData`, `addQWCData`. DeSSE returns 7 values, one per world (inferred). | - | - |
| Summon signs, blue and red | `addSosData`, `getSosData`, `checkSosData`, `summonOtherCharacter`, `outOfBlock` | create/join a room; the server relays its ID | yes |
| Invasions | SOS records of type 3, "Invasion" in DeSSE, so the same relay (inferred) | room | yes |
| Old Monk (a player as the boss) | `summonBlackGhost` | room | yes |
| Ratings after a session | `initializeMultiPlay`, `finalizeMultiPlay`, `updateOtherPlayerGrade`, `getMultiPlayGrade` | - | - |
| Voice chat | - | joins the room | through Avc2 |

#### Wire format (read: DeSSE `emulator.py`, `emu/Util.py`)

- **Bootstrap.** Port 18000 returns `ss.info` as Base64. In DeSSE that file holds:
  - `gameurl1` to `gameurl12` entries (`http://<ip>:<port>/cgi-bin/`);
  - polling intervals;
  - ghost and message counts.
- **Game servers.** Port 18666 (US), 18667 (EU) and 18668 (JP). The server tells regions apart by port.
- **Requests.** POSTs whose body is `key=value&...`, encrypted with AES-256-CBC under a fixed key, with a 16-byte IV in front.
- **Responses.** One command byte, a little-endian length and the payload, all Base64. `Connection: close`, one request per connection.
- **Players.** `characterID` is the NP online ID plus one slot character (inferred from `initializeCharacter`).
- **Summons.**
  1. The summoner sends `NPRoomID` with `summonOtherCharacter`.
  2. The target polls `checkSosData` and receives the room ID.
  3. The target joins that room through NP.

#### Community servers

- **DeSSE** (ymgve, 2018; Python 2.7 with pycrypto) is the reference emulator.
  - It has no license file, so its code cannot be vendored.
  - Its README says matchmaking worked "only by virtue of Sony's matchmaking servers being online". So it replaces only the DeS server.
  - Its DNS list (`remote.conf`) has six names: `c.demons-souls.com`, `g.demons-souls.com`, `ds-eu-c.scej-online.jp`, `ds-eu-g.scej-online.jp`, `demons-souls.scej-online.jp` and `cmnap.scej-online.jp`.
- **Archstones** is the live private server. It lists no level limits for multiplayer, user-selected and global world tendency, events, and player stats.
  - PS3: set the console's primary DNS to the server's DNS server. The wiki says PS3 play is region-locked. Sign-in and rooms still go through PSN (inferred from DNS being the only change).
  - RPCS3: one game config with Network status "Connected", PSN status "RPCN", DNS 8.8.8.8, and an "IP/Host switches" string. That string maps the same six names to one server IP. Players also need an RPCN account on `np.rpcs3.net`. This server is cross-region.
- So the players are already split in two (inferred). PS3 players meet in PSN rooms and RPCS3 players in RPCN rooms. A port with its own matching server would be a third group.

#### How RPCS3 does it (read, RPCS3 source)

- **Firmware libraries.** It runs the firmware's own libhttp, libnet and libssl: `rpcs3/Emu/Cell/lv2/sys_prx.cpp` lines 80, 100 and 116, where 0 means not HLE. It HLEs netctl, NP, NP2 and Avc2 (lines 101, 137, 138 and 123). Its HLE cellHttp is mostly stubs (227 TODO lines).
- **Redirection** is a DNS hook. `rpcs3/Emu/NP/np_dnshook.cpp` reads the "IP swap list" (`system_config.h` line 334):
  - the format is `host=ip&&host=ip`, with `*` wildcards;
  - it answers the DNS packets the guest's own resolver sends.
- **RPCN.**
  - TLS 1.2 (wolfSSL) to `np.rpcs3.net:31313`.
  - UDP 3657 to the server, for address discovery and hole punching.
  - Protocol version 32, with protobuf messages.
  - Source: `rpcn_config.cpp` line 229, `rpcn_config.h` line 8, `rpcn_client.cpp` lines 263, 1062 and 1082.
- **P2P framing.** P2P datagrams carry a 6-byte header: destination vport, source vport and flags (`rpcs3/Emu/Cell/lv2/sys_net/nt_p2p_port.h`). Vport 0 carries RPCN and signaling messages (`vport0.h`).
- **Size.**
  - `rpcn_client.cpp`: 3,451 lines.
  - `np_handler.cpp`: 1,852.
  - `np_requests.cpp`: 1,967.
  - `signaling_handler.cpp`: 868.
  - lv2 sys_net: 4,155.
- **No DeS-specific code** exists in RPCS3's NP or sys_net. I searched `rpcs3/Emu` for the disc title IDs BLUS30443, BLES00932, BCJS30022 and BCAS20071, and for "demon".

### A3. What ps3recomp has today (read)

Offline is the default.
- `PS3_NET_ONLINE=1` puts real host sockets in front of the offline libnet model (`runtime/ppu/ppu_sysprx.cpp` lines 899-902). It also makes cellNetCtl report a connection (`libs/network/cellNetCtl.c` lines 46-49 and 150-170).
- With `PSNR_SERVER` also set, the NP manager reports ONLINE (`sceNp.c` lines 251-268, `np_psnr.c` lines 98-101).
- psnr (sp00nznet; MIT, Go, v0.1.0 alpha) holds rooms and leaderboards. Game traffic goes peer to peer.
- The boot scaffold already takes `--username` and `--psnr` (`boot_main.cpp` lines 1967-1984). See `ps3recomp/docs/ONLINE.md`.

The coverage of DeS's imports is below. "Resolved" means a handler exists under the SDK name in the current ps3recomp sources.

| Module | Resolved | Missing for DeS | Notes |
|---|---|---|---|
| cellNetCtl | 8 / 8 | - | The net-start dialog always succeeds. The MAC is derived from the player name. |
| sceNp | 26 / 34 | ManagerGetNetworkTime, ManagerGetAvatarUrl, BasicGetMatchingInvitationEntryCount, BasicGetFriendPresenceByIndex, BasicGetBlockListEntry, ScoreRecordGameData, ScoreGetGameData, ScoreGetRankingByNpIdPcId | ONLINE only with psnr. |
| sceNp2 | 24 / 26 | GrantRoomOwner, GetMemoryInfo | ContextStart needs psnr. Without it, the context fails to start with SERVER_NOT_AVAILABLE (`sceNpMatching2.c` lines 799-811). psnr has no owner-change message. |
| sys_net | 23 / 29 | gethostbyaddr, sys_net_if_ctl, sys_net_set_udpp2p_test_param, sys_net_show_ifconfig, sys_net_show_route, sys_net_show_nameserver | Only with `PS3_NET_ONLINE` (`sysNet.c` lines 1037-1063). One datagram P2P socket per instance, with no vport demux (lines 291-295). |
| cellHttp | 8 / 18 | 4 exist under wrong names (next table); ClientSetKeepAlive, ClientSetPerHostPoolSize, ClientPollConnections, ClientCloseAllConnections, ClientSetUserAgent, ClientGetUserAgent | A real HTTP/1.1 client on host sockets: one connection per request, no chunked decoding. Not gated by `PS3_NET_ONLINE`. |
| cellHttpUtil | 1 / 3 | Base64Encoder, Base64Decoder | They exist as Base64Encode and Base64Decode, with other signatures (`cellHttpUtil.c` lines 351 and 390). |
| cellSysutilAvc2 | 7 / 10 | StartVoiceDetection, StopVoiceDetection, SetAttribute | Silent by design: it loads and joins, but carries no audio. |
| **Total** | **97 / 128** | **31** | |

The four cellHttp functions with wrong names (`cellHttp.c`):

| SDK name DeS imports | ps3recomp name | Line |
|---|---|---|
| cellHttpResponseGetStatusCode | cellHttpGetStatusCode | 759 |
| cellHttpResponseGetContentLength | cellHttpGetResponseContentLength | 744 |
| cellHttpRequestSetContentLength | cellHttpSetRequestContentLength | 844 |
| cellHttpTransactionAbortConnection | cellHttpAbortTransaction | 889 |

- **Unregistered imports** return CELL_OK and leave out-params unwritten (`runtime/ppu/ppu_hle.cpp` lines 138-190). For the cellHttp getters, the game would read garbage.
- **NID table.** Handlers are registered from `work/recomp/hle_nid_table.cpp`, which is generated at lift time. Regenerate it after adding handlers.

### A4. What the port needs, stage by stage

#### Stage 0: groundwork (4-5 days; no traffic leaves the PC)

- **Host map (0.5-1 day).** A new knob, `PS3_NET_HOSTS="c.demons-souls.com=203.0.113.5&&..."`, in RPCS3's format with `*` wildcards.
  - Apply it in cellHttp's resolver (`cellHttp.c` line 498) and in sys_net's gethostbyname and gethostbyaddr (`sysNet.c` line 971).
  - Fail closed: the six original names resolve only through the map.
- **Gate cellHttp's sockets** on `PS3_NET_ONLINE`, as sys_net already is (0.25 day).
- **NP signed in without psnr (1 day).**
  - Report ONLINE when `PS3_NET_ONLINE` and a player name are set.
  - Add a `local` matching mode (a new knob, for example `PS3_NP_MATCHING=local`): ContextStart succeeds, and room calls fail cleanly.
  - Whether DeS then goes on to the DeS server is a run question (A8).
- **Small handlers (1 day):** the 8 sceNp, 2 Matching2, 6 sys_net and 3 Avc2 functions.
  - GetNetworkTime returns the host's UTC time.
  - The list getters write zero counts.
  - GrantRoomOwner is a stub here.
- **Settings rows (1 day; A5).**
- **Regenerate the NID table, build, and check that offline play is unchanged (0.5 day).**

#### Stage 1: the shared world (4-7 days, plus 1-2 for a test server)

- **cellHttp (1 day).** Add the 10 functions under their SDK names. The signatures are in RPCS3's `cellHttp.cpp` (lines 1796-3356).
- **cellHttpUtil (0.5 day).** Add Base64Encoder and Base64Decoder with the SDK signature `(out, input, len)`.
- **A test server (1-2 days).** Two options:
  - run DeSSE locally (it needs Python 2.7 and pycrypto; ask before installing);
  - or write a small mock in `tools/` that answers the bootstrap, `login` and `getTimeMessage`.
- **A first traced run (2-4 days).** Run against the test server with `PS3_NET_TRACE=1`. Then fix what the trace shows: keep-alive and poll semantics, partial receives, timeouts.
- **A user-agent marker (0.25 day).** Append something like `ps3recomp` to the string the game passes to `cellHttpClientSetUserAgent`. A server can then tell port clients apart, which Stage 2's summon pool needs.
- **Then, with the operator's OK, Archstones.** On its RPCS3 server, port players' messages, bloodstains and ghosts would land in the same world as RPCS3 players' (inferred).

#### Stage 2: co-op and PvP among port players (8-16 days)

- **GrantRoomOwner end to end (1-2 days):** a new psnr message and the matching server change, made upstream in psnr (MIT).
- **P2P sockets (0-3 days).** Find out how many P2P sockets DeS opens. If it binds two datagram P2P vports, add a demux in `sysNet.c`.
- **NAT (1-2 days).** psnr's punching and relay exist (ps3recomp commits e98be23 and 2205a9b). Test across two networks, not only on loopback.
- **Test harness (1-2 days).** Two instances on one PC, with P2P ports 3658 and 3659 and separate save roots.
- **Play tests (3-5 days).** Blue summon, red summon, invasion, Old Monk, leaving or dying mid-session. Test at 30 fps and at 60.
- **Keep port summon signs away from RPCS3 players on a shared server (1-2 days on our side; needs the operator).**
  - The problem: an RPCS3 player who touches a port player's sign would hand over an RPCN room ID that psnr cannot find. The reverse fails the same way (inferred).
  - The options: a separate server instance, or a server that sorts clients by user agent.
- **A psnr server.** Players could host their own, or one could run next to a DeS server.

#### Stage 3: co-op with RPCS3 players (20-40 days; optional)

- **A clean-room RPCN client.**
  - TLS 1.2: Schannel on Windows, mbedTLS or OpenSSL elsewhere. Not wolfSSL, which is GPL.
  - Account creation with the e-mail token, then login.
  - The protobuf messages for the Matching2 subset DeS uses, plus server notifications.
- **RPCS3's P2P layer:** the 6-byte vport header, signaling over vport 0, and UDP 3657 address discovery.
- **Consent.** Ask the RPCN maintainers before using `np.rpcs3.net`. A separate RPCN instance is allowed by its AGPL license, but its players would not meet RPCS3 users.
- **Licenses.** ps3recomp and the port are MIT. RPCS3 is GPL-2.0 and the RPCN server is AGPL-3.0. Nothing can be copied, and the protobuf schema needs a clean source or the maintainers' blessing.

#### Not planned

- **Voice:** Avc2 audio over P2P plus microphone capture, 10+ days.
- **The roadmap's PvP arena** would build on Stage 2.

### A5. Settings: a Network section

The settings window maps each row to an environment knob (`port/ui/src/settings.cpp`, `kDefs`). All of these need a restart, because NP and sockets start at boot.

Built (the Network tab, between Controls and Interface). You decided Online is on by default. `apply_network` in `settings.cpp` turns the rows into ps3recomp's knobs once at start:

| Row | Setting (default) | Knob it sets, online only |
|---|---|---|
| Online | `DES_ONLINE` (`1`) | `PS3_NET_ONLINE=1`. Offline sets none of the knobs, not even to `0`, since the runtime counts any value as on. |
| Server | `DES_NET_SERVER` (`archstones`) | `PS3_NET_HOSTS`: each of the six names `=` the server's address, joined with `&&` |
| Custom server | `DES_NET_CUSTOM` (empty) | The address for Server's Custom choice: IPv4 or a host name. With no valid address the game stays offline (fails closed). It also stays offline with an address that was not chosen on this PC (see below). |
| Online name | `DES_NET_NAME` (empty) | `PS3_NP_ONLINE_ID`, when it is 3-16 letters, digits, `-` or `_`. Online needs one: empty or invalid keeps the game offline, and the row says so (without an NP sign-in the game asks for the network again and again after Continue). |
| P2P port | `DES_NET_P2P_PORT` (`3658`) | `PS3_NET_P2P_PORT` (1024-65535; anything else means 3658) |

- **The servers** are one table in `settings.hpp` (`kNetServers`: key, name, address, note):
  - The Archstones = `206.189.232.242`, the RPCS3 address its setup page publishes. It is run by a community volunteer.
  - Local test server = `127.0.0.1`.
  - Custom.
- **No psnr row yet.** `PSNR_SERVER` is never set from settings.
- **A custom server must be chosen on this PC.** A portable download repacked with a `settings.ini` that names a server
  of its own would otherwise send everyone who plays it there. So a Custom address counts only once the player picks
  Custom in Server, or types the address in Custom server, on this PC. That stores `net_custom_here` in `settings.ini`:
  a hash of the address and this PC's id (Windows' `MachineGuid`, Linux's `/etc/machine-id`; `report::this_pc`). A
  `settings.ini` copied to another PC does not match there. Until then the game plays offline, says so at start
  ("Playing offline"), and the Custom server row says how to use the address. A `DES_NET_CUSTOM` from the environment
  counts as chosen.
- **The environment wins.** A knob already set in the environment (for example `PS3_NET_HOSTS`) is left alone, as with every other row.
- **Test runs stay offline:**
  - `deslib.sh`'s `des_env_defaults` exports `DES_ONLINE=0` for anything but `play.sh`;
  - `milestones.sh` and `repro.sh` do the same;
  - `apply_network` also stays offline in a harness run (`PS3_PIDFILE` set, not `play.sh`) unless `DES_ONLINE` was given.
- **Cheats win.** `des_dev_boot` runs after the settings are applied (both from `ps3_title_options_init`), so a cheat set at start still clears the network knobs (docs/55 section 6). The tab then shows Online as "Off: cheats used", from `DES_DEV_SESSION`.
- **Issue reports** show only "(set)" for the online name and the custom address.
- **The tab's note:** co-op and invasions show your IP address to the other players; summons need an RPCN account (later).

The first plan, for reference: Online off by default; a free-text game server; a co-op (psnr) server row.

### A6. Legal, terms and safety

- **No Sony service is contacted.** The official servers closed in 2018.
- **Community servers are third parties.** Ask the Archstones operator before shipping a preset or pointing the port at it. A free-text server field is safer than a built-in address.
- **RPCN:** I found no published terms for other clients, so ask first. Licenses are as in Stage 3.
- **DeSSE has no license.** Document it; don't copy it.
- **Nothing from the game is shipped.** The DeS protocol, including its AES key, lives in the user's own lift.
- **Privacy.** P2P shows IP addresses to peers, as on a PS3. psnr has no passwords, so names can be impersonated. The DeS server cannot check names either.
- **Untrusted input.** Servers and peers feed the game's parsers.
  - In January 2022, a remote-code-execution bug in Dark Souls III's P2P code took the PC servers offline (widely reported; no source opened here).
  - Here the PPU code is a static lift, so injected guest code has nothing to run on (inferred; I did not audit the miss path of `ps3_indirect_call`).
  - HLE functions that copy guest-controlled lengths into host buffers still need checking.

### A7. Risks

1. **Signed in, but no rooms.** We don't know how DeS behaves when NP is signed in but Matching2 is unavailable. If it refuses to log in to the DeS server, Stage 1 needs a psnr server too.
2. **Mixed populations on one DeS server.** Summon signs carry room IDs that only one matching network understands (A4, Stage 2).
3. **Names.** psnr does not check them, and the DeS server keys players on them. Two players with the same name would share server-side stats.
4. **The original domains** may now belong to third parties. The host map must fail closed.
5. **HLE semantics.** The cellHttp keep-alive pool and `cellHttpClientPollConnections` may expect asynchronous behaviour that ps3recomp doesn't have.
6. **P2P.** Two datagram P2P vports would need a demux. Behind strict NATs, play would depend on psnr's relay.
7. **Frame rate.** Network timers counted in frames could misbehave above 30 fps (inferred). Test at 30 first.
8. **Save identity.** Setting a player name changes the OpenPSID and the MAC (`lv2_register.c` lines 447-455, `cellNetCtl.c` lines 192-208). Check that the pinned regression save still loads with a name set.
9. **Consent and capacity** of community servers. Licensing and consent for Stage 3.

### A8. Not determined, and the runs that would settle it

Not determined:
- What "(009)" means. My inference is "NP or netctl offline", from the log order.
- Whether DeS logs in to the DeS server when Matching2 cannot start.
- Whether DeS reaches its 13 sceNpScore imports.
- How many P2P sockets and vports DeS opens, and whether it uses stream P2P.
- Whether Archstones' server code is public, and its policy on clients other than RPCS3 and PS3.
- RPCN's policy on other clients.
- Whether PSN matching still works for DeS on a real PS3 today.
- Whether the original domains are registered by someone else today. Not checked, on purpose.

Runs, after Stage 0 has landed:
1. **Stage 1 smoke.**
   - Setup: a mock server on 127.0.0.1:18000, then `PS3_NET_ONLINE=1 PS3_NP_ONLINE_ID=Tester1 PS3_NET_HOSTS=c.demons-souls.com=127.0.0.1 PS3_NP_MATCHING=local PS3_NET_TRACE=1`.
   - Check that the log shows a cellHttp transaction to `c.demons-souls.com:18000/demons-souls-us/ss.info`, then a `login.spd` POST.
   - Check that the title shows the server's message of the day instead of (009).
2. **The same run without `PS3_NP_MATCHING=local`, and no psnr.** This answers risk 1.
3. **Stage 2.** Run two instances against a local psnr, with `PS3_NET_P2P_PORT` set to 3658 and 3659. Watch the `[sys_net]` binds for the number of P2P sockets, then try one blue summon.

## Part B. Linux and macOS: additions to docs/36

docs/36 already covers the native route: the CI, the POSIX shim, the Vulkan and Metal backends, the blockers, the order of work and the AppImage. This part adds only what docs/36 doesn't say.

### B1. Proton or Wine first: what the Windows build needs

The import list was read from `port/build/des_boot.map`. The Proton column is inferred unless marked.

| Windows piece | Used for (read) | Under Proton | Risk |
|---|---|---|---|
| d3d12 + dxgi | the RSX backend: root signature 1.0, occlusion queries, flip-discard, tearing, R24G8 depth | vkd3d-proton | low |
| D3DCompile (d3dcompiler_47) | HLSL `vs_5_0`/`ps_5_0`, compiled at run time for each RSX program (`rsx_d3d12_backend.c` lines 987, 1555, 2410) | Wine's own d3dcompiler_47 compiles through vkd3d-shader. Wine bug reports show it failing on some HLSL, with the native DLL as the usual workaround (search). | **High.** Fix: ship Microsoft's d3dcompiler_47.dll next to the exe. The Windows SDK lists it as redistributable. Wine should then load the native copy (inferred). If not, set `WINEDLLOVERRIDES=d3dcompiler_47=n,b`. |
| D3D11On12CreateDevice (d3d11) | the wrapper UI (`port/ui/src/host.cpp` line 231) | DXVK 2.2 or newer, with vkd3d-proton (search: DXVK 2.2 release coverage) | Medium. If it fails, the game runs without the menu (line 234). |
| Media Foundation (mfplat) | movies, through the MPEG-2 decoder MFT (`ps3recomp/libs/codec/sail_movie.cpp` lines 164-181) | winegstreamer; MPEG-2 video support not confirmed | Medium. Fallback: the FFmpeg path. |
| MSVCP140, VCRUNTIME140 | the C++ runtime | Wine's builtins | Low. Or link the CRT statically. |
| xinput1_4, hid + setupapi | pads; DualShock and DualSense over HID (`pad_hid_sony.c` lines 168-194) | Wine's xinput. HID goes through winebus and needs hidraw access (Steam's udev rules). | low |
| WASAPI + avrt | audio | winepulse or PipeWire | low |
| a vectored exception handler + VirtualAlloc reserve | the 4 GB guest space, with a 64 KB commit on first touch (`boot_main.cpp` lines 1949-1962, 2034-2037) | Works. Each fault costs more than on Windows, but there is only one per 64 KB page touched. | low |
| fibers, SuspendThread, GetThreadContext, waitable timers, timeBeginPeriod | the runtime and the watchdog | supported | low |
| WTSRegisterSessionNotification | lock-screen detection (`rsx_d3d12_backend.c` line 689) | a stub, so the lock is never detected | none |
| dbghelp MiniDumpWriteDump | crash reports | partly implemented | low |

- Plain Wine has its own D3D11 (wined3d) and D3D12 (vkd3d). The menu's D3D11On12 likely needs DXVK plus vkd3d-proton instead (inferred). Proton ships both.
- Steam Deck: add the exe as a non-Steam game and force Proton.

### B2. vkd3d-proton or a native Vulkan backend

- Under Proton, the D3D12 backend runs on Vulkan through vkd3d-proton with every DeS fix intact.
- A native Vulkan path is docs/36's blocker 1: the shared draw engine has never run DeS.
- So Proton is the cheapest route for Linux and the Steam Deck. A native backend matters for macOS and for a native AppImage.

### B3. macOS: Metal, MoltenVK, CrossOver

- **Native:** ps3recomp's Metal backend, or the Vulkan backend over MoltenVK. Both use the shared draw engine (docs/36, blocker 1).
- **CrossOver** runs D3D12 either through its own vkd3d work or through Apple's D3DMetal (an option since CrossOver 23.5; search).
  - Whether either accepts our run-time compiled shader model 5.0 shaders and D3D11On12 is unknown.
  - A 1-2 day test on a Mac would tell.

### B4. Apple Silicon: Rosetta 2 or native arm64

- **Rosetta 2** translates macOS x86-64 programs only. A Windows exe also needs Wine or CrossOver on top.
  - So running "the x86-64 lift under Rosetta" still needs docs/36's macOS port: window, Metal, input, audio and the build.
  - What Rosetta gives: x86 memory ordering for translated code (TSO mode on M-series chips), and the runtime's SSE code runs as is (inferred). The runtime has only ever been debugged under x86 ordering. Concurrency bugs were found even there (the lwarx order and the PUTLLC freezes), so more would likely surface on arm64 (inferred).
- **A native arm64 build of the lift** looks possible. From `work/recomp/ppu_recomp.h`, and `ppu_recomp_000.cpp` lines 120-160 (read):
  - There are no SIMD intrinsics in any lift file. VMX operations are loops over a 16-byte union, and byte swaps go through helpers.
  - `<intrin.h>` and the `_mul128` helpers appear only under `_MSC_VER`. Other compilers use `__int128`.
  - `sync`, `lwsync`, `eieio` and `isync` lower to `PPU_FENCE`, which is a C++ fence. The header says each becomes one `dmb` on arm64.
- **The runtime has a little x86-only code** (counted): `_mm_*` intrinsics in 8 files (mostly SPU), `_mm_pause` in 4, inline asm in 3 and `__cpuid` in 1.
  - sse2neon or SIMDe (both MIT) can map the SSE paths. docs/36 item 6 covers the rest.
- **Extra arm64 work beyond docs/36 (4-7 days, inferred):**
  - a memory-ordering review of the runtime's handoffs: SPU MFC and atomics, the GCM ring, the SPURS queues;
  - long soak runs.

### B5. Small points

- **Big-endian guest memory:** nothing changes on a little-endian host, x86-64 or arm64. The byte swaps stay in `vm_read`/`vm_write`.
- **AppImage:** as in docs/36. If Linux goes the Proton route first, Linux users get the Windows zip, and there is no AppImage until a native build exists.
- **Input, audio, window:** under Proton they work through Wine, as listed in B1. For a native build, see docs/36 items 2 and 3 (SDL2).

## Sources

- DeSSE: https://github.com/ymgve/desse (README, `emulator.py`, `emu/Util.py`, `emu/SOSManager.py`, `emu/PlayerManager.py`, `remote.conf`, `info.ss`)
- Archstones connection guide: https://thearchstones.com/serverinfo.html, and its home page: https://thearchstones.com/
- Demon's Souls wiki: https://www.demonssouls.com/Online_Play_in_2021 and https://www.demonssouls.com/How_to_play_the_Original_Demon%27s_Souls
- RPCS3 with RPCN and DeSSE, 31 July 2020: https://wccftech.com/rpcs3-emulator-updated-to-support-demons-souls-online-multiplayer/amp/
- RPCN server (AGPL-3.0): https://github.com/RipleyTom/rpcn
- psnr (MIT): https://github.com/sp00nznet/psnr
- Server shutdown on 28 February 2018: https://archive.rpgamer.com/news/Q4-2017/112717d.html (from a search summary; the page refused a direct fetch)
- DXVK 2.2 with D3D11On12 (search): https://www.phoronix.com/news/DXVK-2.2-Released
- Wine's d3dcompiler_47 needing the native DLL, one example (search): https://list.winehq.org/hyperkitty/list/wine-bugs@list.winehq.org/thread/AFR33JN6WVLFH3F6FQPOZAXUIBKANGFR/
- CrossOver 23.5 with D3DMetal (search): https://codeweavers.com/about/news/press/20230927
- In this repo: docs/29 (M7, RPCS3's online issues), docs/36 (Linux and macOS), `ps3recomp/docs/ONLINE.md`
