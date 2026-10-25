# Online play on the community servers (research and plan, 2026-10-19)

Status: research, with the first pieces in pull requests. No traffic was sent to any game server or to RPCN while
writing this, and no account was made.

This updates docs/46 (Part A) for the goal you set: play online on the private servers that already exist, with
online on by default, rather than writing a server. docs/46's stage plan still holds; what changed is which stage
matters most. RPCN, docs/46's optional stage 3, is the only way to be summoned by, or invade, the players who are
online today, because they all play through RPCS3 and RPCN. docs/70 covers the RPCN account flow in our UI.

Labels: **read** means seen in source code, the live page, our EBOOT or the disc data. **Inferred** means worked out
but not run.

## Summary

- **Servers.** One public Demon's Souls server is up today: **The Archstones**, run by a volunteer (Yuvi). It is
  closed source, publishes no terms, and serves RPCS3 players from one address in a cross-region pool. Every other
  server is code you run yourself: DeSSE (no licence, Python 2, inactive since 2018), dessego (MIT, Go, 2021) and a
  few dead proofs of concept. DeS Seamless Co-op (GPL-3.0, active) runs a private party server per host.
- **What the game needs from the server.** Plain HTTP; there's no TLS and no NP ticket. The game reaches the server
  through six hostnames that every guide redirects to the server's address. The server handles the shared world:
  messages, bloodstains, ghosts, the sign lists and world tendency.
- **What the game needs besides the server.** A summon or an invasion is brokered by the server, but the session
  itself is an NP Matching2 room plus NP signalling. On RPCS3 both run on **RPCN** (`np.rpcs3.net`), and gameplay
  then flows peer to peer over UDP. So messages and bloodstains need only the server; co-op and PvP with RPCS3
  players need an RPCN client too.
- **What our runtime is missing** (docs/46 A3, re-checked tonight):
  - a host map for the six names;
  - working cellHttp (four guest-pointer bugs, so every request was wrong);
  - NP signed in without ps3recomp's own psnr server;
  - an offline lock for the developer cheats;
  - and, for summons, an RPCN client, RPCN's P2P framing on port 3658, and Matching2 and signalling over RPCN.
- **Password matchmaking.** No server has it. A client-only filter can hide signs from anyone outside your RPCN
  friends list, and refuse summons from them, but it can't stop others from seeing your sign. A real password
  needs the server: a few lines in `getSosData`, which only Yuvi can add to Archstones. Recommendation in section 5.
- **Built tonight (pull requests):**
  - the ps3recomp network groundwork (host map, cellHttp fixes, NP local sign-in, offline lock);
  - the Network tab in Settings, online on by default (online needs an online name; see below);
  - per-copy save and cache folders, and `tools/des/duo.sh` for two copies at once;
  - `DES_GIVE` (developer builds), `tools/des/online_save.sh` and a test save with every multiplayer item.
- **First run in the game, local only** (section 4a). The six names went to 127.0.0.1, where a listener logged
  what arrived; nothing went to any real server.
  - Online with no online name: NP stays signed out, and the game asks for the network over and over after
    Continue and never loads the level. The Network tab now keeps the game offline until a name is set.
  - Online with a name: NP signs in and the level loads. After the cellHttp fixes (ps3recomp #81) the game sends
    its bootstrap, `POST /demons-souls-us/ss.info`, to `c.demons-souls.com:18000` with the user agent a PS3 sends.
  - With a local stub answering the bootstrap, the game reads the reply and sends its encrypted login
    (`login.spd`) to the game server address in it. That is as far as the client can go without a real server.

## 1. The servers

| Server | State | Source and licence | Notes |
|---|---|---|---|
| The Archstones (thearchstones.com) | Website up (page dated 2022-12-01). Game ports not probed. Likely up: every 2025-2026 guide points to it. | Closed. Only the admin tool (Yu-Dessmt) is public, MIT. | RPCS3 pool at 206.189.232.242, cross-region. PS3 players use the DNS server 142.93.245.186 and are split NA/EU/JP. Donation-funded. No terms published; the Demon's Souls Discord is the contact. |
| DeSSE (github.com/ymgve/desse) | Code only | **No licence** (all rights reserved) | Python 2.6/2.7 and pycrypto. US, EU and JP on ports 18666, 18667, 18668; bootstrap on 18000. The reference every other server follows. |
| dessego (github.com/danmrichards/dessego) | Code only, 2021 | MIT | DeSSE in Go with SQLite. The best base for a local test server. |
| DeS Seamless Co-op (github.com/himingal/des-seamless-coop) | Active (September 2026) | GPL-3.0 | .NET app for RPCS3. Each host runs a private server; a friend joins with party name and password, through a relay if needed. Merges the three regions. |
| Others | Dead | Mostly MIT | Asapin (Java, EU), Przytua (Swift), moefh (Go), jpdante (C#), desse-docker |

**What the client needs (read: DeSSE and dessego source, our EBOOT).**
- **Hostnames.** `c.demons-souls.com` (US bootstrap), `g.demons-souls.com`, `ds-eu-c.scej-online.jp`,
  `ds-eu-g.scej-online.jp`, `cmnap.scej-online.jp` (EU bootstrap, per the Asapin README) and
  `demons-souls.scej-online.jp` (JP, inferred). Our EBOOT (BLUS30443) has the US bootstrap
  `c.demons-souls.com:18000`, path `demons-souls-us/ss.info`, and the formats `http://%s/%s` and `%s%s.spd`.
- **Protocol.** The bootstrap returns `ss.info`, which points the game at `http://<ip>:18666/cgi-bin/` for the US.
  Requests are POSTs to `<name>.spd`. The body is AES-256-CBC with a fixed key and the IV in front, done by the game's
  own code. Replies are base64 of `[cmd u8][len u32][data]`.
  - Endpoints: login, character, messages, replays (ghosts), wandering ghosts, sign lists (`addSosData`,
    `getSosData`, `checkSosData`), `summonOtherCharacter`, `summonBlackGhost`, multiplayer start and finish, and
    grades.
- **Identity.** No server checks a ticket, and the game imports no `sceNpManagerRequestTicket`. Players are known
  by the plaintext character ID, built from the NPID plus a slot digit. Closed Archstones may do more; unknown.
- **Regions.** RPCN maps DeS's communication IDs (`NPWR00881` US, `NPWR00245`) to the EU one (`NPWR01249`), so all
  regions meet in one Matching2 pool (read: RPCN's `server_redirs.cfg`).

**Summons, step by step (read: DeSSE `SOSManager.py`; inferred for the room part).**
1. A player with a Blue Eye Stone posts a sign: `addSosData` (block, position, level, message, type).
2. Another player in the same block lists signs (`getSosData`) and touches one. Their game creates a Matching2 room
   and sends its room ID with `summonOtherCharacter`.
3. The sign's owner picks the room ID up through `checkSosData`, polled every few seconds, and joins the room.
4. NP signalling connects the two, and the game's own UDP P2P carries the session. Invasions (Black Eye Stone) and
   the Old Monk's summon (`summonBlackGhost`) run the same way.

Steps 1-3 talk to the private server; step 4 and the room in step 2 go to RPCN.

**How RPCS3 players set it up (Archstones' Server Info page, quoted).**
1. In a custom game config: "Set Network Status to Connected", "Set PSN Status to RPCN", "Set DNS to 8.8.8.8".
2. Set "IP/Host Switch" to the six names `=206.189.232.242`, joined by `&&`.
3. In Configuration > RPCN: host `np.rpcs3.net`, create an account, and enter the emailed token.

Your RPCS3 config for BLUS30443 already has this exact switch string, with PSN status Disconnected.

## 2. How RPCS3 connects (read: RPCS3 source, contracts only)

RPCS3 is GPL-2.0, so nothing is copied; this lists what a compatible client must do.

- **Host overrides.** `IP swap list` takes `host=ipv4&&host=ipv4`. Names match case-insensitively, `*` is a
  wildcard, and the first match wins.
  - RPCS3 runs the console's own libnet and libhttp, so it catches the game's DNS queries at the socket level: UDP
    to port 53, answered with a made-up A record.
  - Our runtime implements libnet and libhttp itself, and resolves names with the host OS (sys_net
    `gethostbyname` and cellHttp). The map goes into that one resolver instead; no DNS packets need faking.
- **NP sign-in.** "Online" means the network is enabled and the RPCN login succeeded.
  - The NPID is the RPCN username.
  - The public address comes from RPCN's UDP echo on port 3657, sent from the P2P socket.
  - Status is ONLINE or OFFLINE; offline, the manager calls return `SCE_NP_ERROR_OFFLINE`.
- **P2P.** Every P2P socket the game opens shares one UDP socket on port 3658. Each datagram carries a 6-byte
  header: destination vport, source vport, flags. Vport 0 carries RPCN's address replies and signalling.
- **Signalling.** RPCN tells each member the others' addresses, in the join reply and in "user joined"
  notifications.
  - Peers then shake hands directly: a `SIGN` v3 packet with CONNECT, CONNECT_ACK and CONFIRM, repeated every
    200 ms.
  - They ping every 10 s, and a peer counts as gone after 60 s without traffic.
- **Matching2.** Every request (create, join, search, room data, leave) becomes an RPCN command, and the reply comes
  back as an event the game reads.
  - RPCS3 stubs KickoutRoomMember and GrantRoomOwner; DeS imports both.
- **RPCN.** docs/70 has the protocol in full: TLS on 31313, protocol version 32, a 15-byte header, and a password
  derived with PBKDF2-SHA3-256.

## 3. What our runtime has and lacks

DeS imports 128 network functions; 97 have a handler (docs/46 A3; the NID map is re-checked).

| Piece | Today | Needed | Where |
|---|---|---|---|
| On/off | `PS3_NET_ONLINE` env only | A settings row; an offline lock for cheat sessions | Network tab PR; ps3recomp `ps3_net_lock_offline()` |
| Host redirect | None. cellHttp used the OS resolver even offline | A fail-closed `PS3_NET_HOSTS` map in the one resolver, and cellHttp offline unless online. IPv4 literals pass: the bootstrap reply gives the game server as an IP | ps3recomp `feat/net-host-map` |
| cellHttp | Path read at the wrong offset (every request `GET /`), POST body and receive buffer used raw guest addresses, guest pointers printed as strings | Fixed (two match upstream fixes, two are new) | same |
| NP sign-in | ONLINE only with ps3recomp's psnr server | ONLINE with online on and a name ("local"): the game's server login can run | same |
| Online name | Optional | Required: without an NP sign-in the game loops on the net-start dialog after Continue (run, section 4a) | Network tab PR (offline until a name is set) |
| cellHttp coverage | 8 of DeS's 18 cellHttp imports, 1 of 3 cellHttpUtil | All 18 and all 3, with ids from 1, the firmware's error values and `ParseUri` in the SDK's layout (section 4a). RPCS3 doesn't need them in HLE: it loads the firmware's libhttp.sprx and libnet.sprx | ps3recomp #81 |
| Identity | Read before settings.ini is applied, so a name in Settings was ignored | Read at first use | same |
| Net-start dialog | Always "connected" | Fails when offline | same |
| P2P | Plain UDP/TCP on 3658, no vport header | RPCN's vport framing, to talk to RPCS3 peers | stage 3 |
| Matching2 and signalling | Over psnr only | Over RPCN, with GrantRoomOwner and Kickout | stage 3 |
| RPCN client | None | TLS, version 32, account commands, UDP echo, friends, presence | stage 3; docs/70 |
| Scores, Avc2 voice | Partly stubbed | Not needed to play | later |

## 4. The plan, by what each step gets you

1. **Groundwork (pull requests tonight).** The host map, cellHttp, NP local sign-in and the Network tab. In local
   runs the game now sends its bootstrap and, given a reply, its login (section 4a). Whether messages, bloodstains,
   ghosts and world tendency then work needs a real server (step 2); not run yet.
   - Summons stay off: Matching2 refuses to start without psnr or RPCN, so the game says it can't connect for
     co-op.
2. **Test it locally first.** dessego (MIT) on 127.0.0.1, the "Local test server" preset, two copies with
   `tools/des/duo.sh`. Needs Go (not installed; your call).
3. **Then Archstones**, after asking Yuvi (section 6).
4. **Co-op with RPCS3 players: an RPCN client** in ps3recomp, clean-room from the contracts above, about 20-40 days
   (docs/46 stage 3). Order:
   1. login and the UDP echo;
   2. the vport framing on 3658;
   3. Matching2 create, join, search and leave, and the notifications;
   4. SIGN v3 signalling;
   5. friends and presence.
   It needs RPCN's maintainers to agree to another client on `np.rpcs3.net` (docs/70 section 2).
5. **Stage 2 of docs/46 (psnr co-op among port players only)** becomes a fallback, not a step. It would split the
   few players there are, and an RPCS3 player touching a port player's sign would hand over a room psnr can't find.

## 4a. The first local run (read: logs of runs on this PC, 2026-10-19 night)

The combined build (both ps3recomp branches, the Network tab, `DES_GIVE`) ran on a copy of the test save. All six
names were mapped to 127.0.0.1 with `PS3_NET_HOSTS`; any other name was refused by the fail-closed map. A small
listener on 127.0.0.1:18000 (the bootstrap port) logged every request. Nothing was sent to The Archstones or RPCN.

| Run | What happened |
|---|---|
| Online, no online name | `sceNpManagerGetStatus` OFFLINE, one OFFLINE callback. After Continue the game called `cellNetCtlNetStartDialogLoadAsync`/`UnloadAsync` 261 times in 4 minutes ("connected" each time) and never loaded the level. |
| Online, name `TestA` | NP ONLINE, Matching2 context created, the level loaded. During the load the game initialised cellHttp, created a client and set it up: `ClientSetKeepAlive`, `ClientSetPerHostPoolSize`, `ClientGetUserAgent`, `ClientSetUserAgent` (all unregistered, faked as CELL_OK). It started `DSNetworkService::Thread`, which exited at once with status 0, then closed the connections and ended cellHttp. No transaction was created, no name was resolved and no request reached the listener. |
| Same, with the cellHttp functions and ids from 1 | The game set its user agent (`PS3Application libhttp/1.0.0-000 (J-JP9000-BLUS30443_00-DS01040000000100/0000000100)`) and the thread polled twice, then still exited. A trace of the HLE calls after `ClientSetUserAgent` showed its last call: `cellHttpUtilParseUri(uri=NULL, ...)`. |
| Same, with `ParseUri` in the SDK's layout | The bootstrap went out: `POST /demons-souls-us/ss.info`, 32-byte body, to `c.demons-souls.com:18000` (mapped to 127.0.0.1). The listener answered 404 and the game closed its client. |
| Same, with a local stub answering the bootstrap | The stub sent back a bootstrap pointing at 127.0.0.1:18666. The game read it and sent `POST http://127.0.0.1:18666/cgi-bin/login.spd`, 64 bytes, AES-encrypted with the game's fixed key. Decrypted: `NPID=TestA&rang=language15&region=non&ver=100&`. The first try was refused because the host map blocked IPv4 literals; fixed. |

What it means:

- **The online name is required.** The Network tab now keeps a session offline without a valid name, and says so
  on the row and in "This session".
- **The client reaches the server's login.** Three runtime gaps stood in the way, each found from a run:
  1. client id 0: the game reads 0 as "no client" (it checks at 0x00544CF8);
  2. `cellHttpUtilParseUri`: the game calls it twice before every transaction, first with `uri` NULL to learn the
     pool size (call site 0x0054D764), then with a pool of that size and a 24-byte `CellHttpUri`. Ours wrote its
     own 2.7 KB struct and refused the size query;
  3. the host map refused IPv4 literals, which is how the bootstrap names the game server.
- **How the game's network thread works** (read: our EBOOT). `DSNetworkService` (vtable 0x018606B0) runs
  `while (!step()) usleep(66666)` (0x0056FCA0). The step (0x00550B00) polls connections and runs the request
  phases until its done byte is set. A poll error other than "no connection" (0x80710012) ends it.
- **The stub is not a server.** It answers the bootstrap and logs what follows. Past the login, testing needs
  dessego or DeSSE running locally, or Archstones (decisions 1 and 3).
- **For the port, after merging ps3recomp #81:** regenerate the lift's HLE NID table
  (`python ps3recomp/tools/gen_hle_nids.py --all --out <lift>/hle_nid_table.cpp`). Without that the 13 new
  functions stay unregistered and are faked as CELL_OK, and the game's network thread quits as before.

## 5. Password matchmaking and world tendency (built 2026-10-20)

Online play runs on a server of our own (dessego), so both are ours to add.

### Matchmaking password

The Network tab's **Matchmaking password** (DES_NET_PASSWORD) puts the player in a group with everyone who set the
same password:
- **Matched within the group:** summon signs, invasions (a Black Eye stone puts down a hidden black sign that hosts'
  games summon) and the Old Monk.
- **Shared with everyone, as before:** messages, bloodstains, ghosts, replays and the world tendency average.
- **Empty** (the default) is the public group, the game's own behaviour.

How it works:
- **Sending.** At start, `apply_network` (port/ui/src/settings.cpp) sets ps3recomp's `PS3_HTTP_HEADER` to
  `X-DeS-Options: group=<hash>`. cellHttp adds that header to every request. The hash is the first 128 bits of
  SHA-256("des-match-group:" + password), so the password never leaves the PC. The header is key=value pairs
  separated by `;`, so later options can join it.
- **Live.** A changed password applies at once: `apply_match_group` sets the new header, and cellHttp reads
  `PS3_HTTP_HEADER` for each request. A sign already down moves along: its owner's game keeps asking
  `checkSosData` while the sign is down, and dessego moves the sign to the group that request carries (a summon
  agreed in the old group is dropped). A `PS3_HTTP_HEADER` from the environment wins over the row.
- **Matching.** Our dessego fork (JonathanDC64/dessego) reads `group=` and keeps it with each sign. `getSosData`
  lists only the requester's group, and `summonOtherCharacter` and `summonBlackGhost` match only within it. A client
  without the header (RPCS3, a PS3, an older port) is in the public group.
- **Other servers** ignore the header, and everyone matches as before.

A hash only keeps the password itself off the server. Anyone who guesses the password joins the group, so it keeps
strangers out; it is not a secret against someone who knows it.

### World tendency

The Network tab's **World tendency** (DES_NET_TENDENCY): "With the server's" (the default) or "Yours only".
Addresses are from the EBOOT (static analysis, 2026-10-20):
- The game's QWC object (`*(*0x01B4EF9C + 0x24)`) keeps the player's own value per area at +0x48 + 8 * (area - 1),
  two floats (white/black, left/right).
- **Once per load:** 0x009204A0, online, sets each area to (1 - 0.5) * own + 0.5 * server and stores that as the
  own value, so the mix is saved. The rate is +0x108 (0.5).
- **New character, and after the login:** 0x00920FC8 sets each area to the server's value.
- **Offline:** both use the own values alone.
- **The hook.** Both read the server's values from the `getQWCData` reply through 0x00567E98 (r4 = out, r5 = area).
  `des_qwc_local`, after the `mflr` at 0x00567E9C, writes the own values to out and clears r4, so the reply is
  skipped. With server = own, both functions give the offline result.
- **Unchanged:** the player's own values still go to the server (`addQWCData`). As a phantom, the host's world comes
  over P2P (0x00525750) and is untouched. A mix saved earlier stays.

Not settled statically: when the event script calls the once-per-load blend. Whether 0x00920FC8 can run after a save
has loaded is also open.

### Message ratings

A rating reaches its message's author late, by design of the client (read: EBOOT disassembly, 2026-10-20):
- **The author.** Only the `getBloodMessageGrade` reply (the player's total rating) tells the game. A step that runs
  each frame (0x0049BFF8) compares the total with the one it saw last. When it went up, the game heals the player
  and shows that a message was rated. The `getBloodMessage` reply never feeds that check.
- **When it asks.** The network scheduler (0x0054E7E8) asks at the login, on its first tick, then every 300 s (the
  0x12C at 0x0054E938). An area change fetches messages again but not the grade. The server cannot push anything:
  the client only acts on replies to its own requests. So on the PS3 too, a rating showed up to 5 minutes later.
- **The rater.** Rating a message adds 1 to the game's own copy (0x003863E0) and sends `updateBloodMessageGrade`.
  A message already open on screen keeps the number it opened with; read again, it shows the new one.
- **The option.** The Network tab's **Message ratings** (DES_NET_RATING_POLL): "Every 5 minutes" (the game's) or
  "Every 30 seconds". `des_net_grade_poll`, after the `subf` at 0x0054E934 (r0 = seconds since the last ask),
  makes the scheduler ask from that many seconds on. Live.

### Showing messages, bloodstains and ghosts

The Network tab's **In your world** rows (DES_NET_MESSAGES, DES_NET_BLOODSTAINS, DES_NET_GHOSTS, all on by
default) hide what other players put in your world. Read from the EBOOT, 2026-10-20:
- **What is hidden.** The requests and uploads go on as before; `des_net_show` takes the lists as empty:
  - messages: the `getBloodMessage` parser 0x00562A98, its entry count in r21 (0x00562BC4). The player's own
    messages are hidden too (a keep-own filter needs three hooks with condition-register edits; not worth it);
  - bloodstains: the `getReplayList` parser 0x005607D0, its count in r18 (0x00560904). The soul bloodstain is
    the game's own (GameDataMan) and stays;
  - ghosts: the ghost tick 0x003A5808's "start a ghost" flag (r29 at 0x003A5CA8). Ghosts already walking finish.
- **Empty is the game's own path.** A zero count removes the records shown before, through the same removal lists
  the game uses when the server's list changes. An empty list is cached under the key -1, so the block's real list
  stays cached and comes back when the row is switched on.
- **At once.** Both lists have a per-block cache that lasts 30 minutes, and the scheduler parses them again every
  300 s and at area changes, from that cache while it is fresh (so the "An area change fetches messages again"
  above is from the cache). When a row changes, the hook zeroes that list's last time in the scheduler (0x0054E8C0
  bloodstains, 0x0054E8F8 messages), so it is parsed again on the next tick.

## 6. Safety, consent and defaults

- **Online on by default** (your decision). Test runs stay offline: `deslib.sh` sets `DES_ONLINE=0` for every test
  kind, and a developer-cheat session clears the network knobs, as before.
- **Default server** (changed 2026-10-25). The project's own server is the first preset and the default. Until it
  has a host name (`kNetProjectHost` in port/ui/src/settings.hpp), online stays off with it: "not available
  yet". The Archstones is a preset in developer builds only. In a release build a saved `archstones` reads as
  the default, and Custom still takes any address.
- **Sign-in** (2026-10-25, ps3recomp's docs/ONLINE.md "Sign-in"). With the project's own server, Local test server
  and Custom, the game signs in to the server with a key of this install, `online-key.bin` in the data folder
  (`net_key_file`), made at the first sign-in. The first key to sign in with a name owns that name there.
  `apply_network` sets `PS3_NET_SIGNIN=<server>:18000`, `PS3_NET_KEY_FILE`, `PS3_NET_CLIENT_VERSION` (this
  build's version) and `PS3_NET_NETWORK_VERSION`; with the project's own server also `PSNR_SERVER=<server>:36100`.
  A server with no sign-in (the old ones) is used as before. A refusal (a ban, maintenance, an old build) shows
  as an "Online" toast with the server's words (`port/ui/src/net_notices.cpp`). The server's banners
  (`GET /dss/v1/banners`, asked every 5 minutes) show the same way, each once. A test run keeps no data folder
  and so does not sign in, unless it is given one (`DES_DATA_DIR`, as `duo.sh` does).
  Checked 2026-10-25 against a local copy of the new server on 127.0.0.2, sessions required:
  `DUO_TEST=1 tools/des/duo.sh start a DES_ONLINE=1 DES_NET_SERVER=custom DES_NET_CUSTOM=127.0.0.2
  DES_NET_NAME=<name> PSNR_SERVER=127.0.0.2:36100`. The game made its key, signed in, psnr took the session,
  and every game request came back 200. With the server in maintenance the toast said so and the game went on
  offline.
- **IP addresses.** P2P shares your public IP with the players you meet, as on a PS3 and on RPCS3. The Network tab
  says so.
- **Cheats.** docs/55's offline rule now has the ps3recomp lock it was missing: once cheats are used, the network
  stays off for the session in every module.

## 7. Test tools made tonight

- **Two copies at once:** `tools/des/duo.sh start` (section 4 of its header has the knobs). Each copy gets its own
  saves (`PS3_SAVEDATA_ROOT`), HDD cache (`PS3_HDD1_ROOT`), shader cache, settings.ini and so its own Network
  settings and later its own RPCN account, logs, console file and P2P port (3658 and 3659). The cores are split
  between them. They share the controllers, so give one no pad (`B_PS3_NO_USER_INPUT=1`) or play it with
  keyboard and mouse.
- **The test save.** `DES_GIVE=online` (developer builds) adds, through the game's own add-item (0x008FB4F0):
  - 10 Stones of Ephemeral Eyes (goods 1021);
  - the Black, Red, Blue and White Eye Stones (9995, 9996, 9997, 9999);
  - the "Kick Item" (9998), which sends phantoms home;
  - the Friend's and Foe's Rings (rings 114, 115).

  IDs read from the disc's goods table and names. The give waits until the character's stats are loaded and no
  cutscene runs, then asks the game to save through its own SaveRequest script function (0x004418B8).
  `tools/des/online_save.sh` does the whole thing on a scratch copy of the pinned regression save. The result is at
  `work/tmp/online/save`, apart from your save and the pinned save. Checked: all eight items in the save file the
  game wrote, and a fresh boot of a copy loads that file.
- **Two copies, checked:** `DUO_TEST=1 DUO_SAVE=work/tmp/online/save tools/des/duo.sh start` started both copies on
  separate cores. Each loaded the test save from its own folder and reached the level. `duo.sh stop` stopped only
  those two. Each copy has to run from the repository folder, because the runtime reads some files relative to it
  (`work/libsre/libsre_image.bin`).

## 7a. Remote players: why a stop shows late (read: EBOOT disassembly and two-copy traces, 2026-10-20)

In co-op between two local copies, a player who stops keeps walking on the other screen for most of a second. The
transport is not the cause: in a traced run every datagram arrived within about 10 ms, with 13 of 3014 lost. The
game's own sync makes the delay. The addresses below come from static analysis of the EBOOT, and the send rate was
checked against the trace.

**Sending.** The local player's PadManipulator sends a type-1 packet with its position, rotation and HP
(0x00313AD0, through 0x00525B70). The packet has no timestamp, no speed and no animation state. A counter at +0x198
counts down once a frame, sends at 0, and reloads with `li r0, 5` at 0x00313B24. One packet goes out every 6 frames:
5 a second at 30 fps. The trace agrees: the 52-byte datagrams left 200 ms apart.

**Receiving.** The NetworkManipulator update (0x0030C870) drains every queued type-1 packet each frame. For each one
(0x0030C090) it sets the chase speed to the distance left divided by 1.2 times the time since the previous packet,
counted with its own frame times. A jump faster than 20 m/s, or a gap over 2 s, snaps instead. Each frame the body
moves at that speed toward the last position, and lands on it only when one more step would pass it. So it covers
5/6 of each gap's distance before the next packet arrives.

**The animation.** SetMoveVector (0x0030C378) picks idle, walk, run or dash from the frame's move times 30 (the
fmuls at 0x0030C46C), against 0.01, 2 and 6. When the sender stops, the stop is seen with the next packet, up to
200 ms later. Each later packet then leaves a sixth of the distance, and the walk plays until the chase drops under
0.01 m/s: about 0.7 s for a walk and 0.8 s for a run, by a simple model. This is the game's behaviour on a PS3 too,
inferred from the code; it was not compared with a PS3.

**What the port changes** (`des_net_sync`, port/des_options.cpp):
- **DES_NET_SYNC=n** (Network tab, "Your movement updates"): this copy sends n packets a second, at most every
  second frame. 15 at 30 fps is every other frame, and the tail on the other screen drops to about a quarter of a
  second. It changes what this copy sends, so each player needs it for the others to see them smoother. Off by
  default (the original rate). Receivers take any rate, since they time the gap themselves.
- **Above 30 fps** the move-times-30 product is not a speed: a running remote showed the walk. With DES_FPS set, the
  hook scales it by 1/(30 dt). It does nothing at 30 fps.
- **Packets under 150 ms apart** (a sender above 30 fps, or DES_NET_SYNC) broke the game's chase in three ways, all
  seen in a two-copy log at 110 fps with packets about 45 ms apart:
  - *The gait flickered.* The receiver counts each gap in its own frames, so a gap is off by up to one frame:
    20-30% of 45 ms. The chase speed then jumps at every packet, and an early arrival drops the move under 0.01
    for a frame, which picks idle at once (the other gaits wait 0.2 s). While packets come that often and the
    sender moves, the gait comes from the sender's own speed instead: the distance between its last two positions
    over their gap, averaged with the one before. That is the speed the chase settles to.
  - *The speed surged.* The gap the chase divides by (+0x21C, at 0x0030C330) is replaced with an exponential
    average of the recent gaps (a fifth of each new one).
  - *The stick let go.* A chase that reaches the last position before the next packet moves nothing until it, and
    SetMoveVector turns a zero move into a zero stick. While the sender moves, such a frame gets a 0.1 mm move
    along the last direction, so the stick holds.

  In a replay of that log, the speed jumps at packets fell from p90 1.6 m/s to 0.6, and the zero-move frames from
  29 to none, with the same lag (5 cm on average). At the game's own rate (30 fps, 200 ms) none of this applies.
  DES_NETFIX=0 turns all of the receiving side off. DES_NETLOG=1 logs each remote's chase every frame.
- A copy running below 30 fps sends less often, and the others see it stop later. This is why one slow copy on a
  shared PC looks laggier than the other.

Not settled: which thread polls the socket, and whether the remote body moves by the move vector or by root motion
(the design suggests the move vector).

## 7b. Wandering ghosts at high frame rates (read: EBOOT disassembly, 2026-10-20)

Online, other players' ghosts walked far too fast above 30 fps.
- **Recording** (the sender) runs on dt: ReplayRecorder (PlayerIns + 0x640, 0x002E4C68) adds a sample once 1/3 s
  has passed. A sample is 32 bytes (position, rotation, two state ids) with no time in it, and a blob holds up to 27.
  At 144 fps the samples are 0.340 s apart instead of 0.367 s, so uploads stay as on the PS3.
- **Playback** (the receiver) runs on frames: ReplayManipulator's Update (0x00314C80) moves on to the next sample
  every 10 frames (0x00315030), and 0x003146F0 splits the move and the turn to it into 10 per-frame steps (the 10.0
  at 0x003147EC). SetMoveVector picks walk or run from the step times 30 (0x00314ABC).
- **The fix**, `des_ghost_rate`: with DES_FPS set, N = 1/(3 dt) frames a sample, N as the divisor and 3 N as the
  speed factor. All three are 10, 10 and 30 at 30 fps. DES_GHOSTFIX=0 turns it off. The constants are not patched in
  place: the same 10.0 is also a distance test at 0x00314DC0.
- Bloodstain replays and the `BeginReplayGhost` event most likely play through the same pair (ReplayGhostIns calls
  SetReplay 0x003145D8), so they get the same fix. Not checked in a run.

## 8. Decisions for you

1. **Archstones as the default server** (settled 2026-10-25: the project's own server, section 6): ask Yuvi first
   (Demon's Souls Discord), or ship the preset now?
   Recommended: ask first, and until he answers keep the preset but default to **no server**, so a fresh install
   doesn't connect anywhere. That partly undoes "online on by default".
2. **RPCN:** ask RPCN's maintainers (RipleyTom) whether a third-party client may use `np.rpcs3.net` (docs/70
   section 2). Nothing in stage 4 should start before that answer.
3. **A local test server:** install Go to run dessego (MIT) on 127.0.0.1, or Python 2.7 and pycrypto for DeSSE (no
   licence; local use only)? Recommended: Go and dessego. The client now reaches the login (section 4a), so this
   is the next test.
4. **Password matchmaking:** the client-side friends filter with the RPCN client, plus a request to Yuvi for a
   keyword filter (section 5)?
5. **Region:** the port is BLUS30443 only today. All six hostnames are mapped anyway, so EU and JP discs would work
   too once supported.
6. **The bootstrap stub:** keep it as a tooling script (`tools/des/net_stub.py` with the local host test), or leave
   it in scratch? It answers only the bootstrap and logs the rest, so it is not a server. Recommended: commit it;
   it checks the client side in a minute without any install.

## Sources

- docs/46 Part A; docs/55 section 6; docs/47 (the inventory).
- github.com/ymgve/desse, github.com/danmrichards/dessego, github.com/himingal/des-seamless-coop,
  github.com/Asapin/ds-server-PoC.
- thearchstones.com and its serverinfo page; demonssouls.com "Online Play in 2021"; nexusmods.com demonssouls mod 189.
- github.com/RipleyTom/rpcn (`server_redirs.cfg`; AGPL-3.0).
- RPCS3 source (local reference checkout, HEAD 065b490): `Emu/NP/np_dnshook.cpp`, `np_handler.cpp`,
  `rpcn_client.cpp`, `signaling_handler.cpp`, `Emu/Cell/lv2/sys_net/*`, `Emu/Cell/Modules/sceNp*.cpp`,
  `cellNetCtl.cpp`, `rpcs3qt/rpcn_settings_dialog.cpp`.
