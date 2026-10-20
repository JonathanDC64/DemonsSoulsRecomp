# RPCN accounts in our UI (design, 2026-10-19)

Status: design only. No account was made, and nothing was sent to RPCN's servers. Doing either is your call.

RPCN is RPCS3's replacement for PSN. Co-op and invasions with the players online today need an RPCN account and an
RPCN client in the port (docs/69 sections 1 and 4). This covers putting RPCN's account flow into our own UI: create
an account, verify the emailed token, sign in, and reset the password. It also covers making that an optional step
of the first-run setup (docs/52), storing the credentials, and two copies with two accounts.

The protocol facts come from RPCS3's client (`rpcs3/Emu/NP/rpcn_client.cpp`, `rpcn_config.cpp`,
`rpcs3qt/rpcn_settings_dialog.cpp`; local reference checkout, HEAD 065b490). RPCS3 is GPL-2.0 and the RPCN server
(github.com/RipleyTom/rpcn) is AGPL-3.0. Our code is MIT, so this describes contracts only, and the implementation
is written from them.

## Answer first

- **Feasible.** The account flow is five plain commands over RPCN's normal connection: Create, Login, SendToken (resend
  the email token), SendResetToken and ResetPassword. RPCS3's dialogs are thin wrappers over them, and the same
  screens fit in our Network tab.
- **Compatibility detail.** RPCS3 never sends the typed password. It sends a key derived from it:
  - PBKDF2 with SHA3-256, 200,000 iterations and a fixed salt string;
  - written as 64 upper-case hex characters.

  We must derive it the same way, or an account made in RPCS3 won't sign in from the port, and the reverse.
- **Permission is the open question.** The only thing that identifies a client to RPCN is its protocol version
  (32). RPCN publishes no terms for other clients, and the server is AGPL, so running our own instance is allowed.
  Using `np.rpcs3.net`, where the players are, needs the maintainers' agreement. **Recommendation: ask before
  writing the client** (decision 1). A client that pretends to be RPCS3 against their wishes would hurt the
  community the port depends on.
- **Storage.** Keep only the derived key, not the password, as RPCS3 does, plus Windows' per-user encryption (DPAPI)
  on top. Keep it in the copy's data folder, never in logs, reports or the repository.
- **Two copies** each have their own data folder (`DES_DATA_DIR`, set by `tools/des/duo.sh`), so each has its own
  account file. RPCN allows one session per account, so two copies need two accounts. You'd make those.
- **First run.** Add an optional "Online account" step after Shaders, before Done, with Skip as the default button,
  and the same screens in Settings > Network.

## 1. The protocol (contracts)

**Connection.**
- TCP to `np.rpcs3.net:31313` (the host is a setting), then TLS 1.2.
- **RPCS3 turns certificate checking off.** The server then sends a ServerInfo packet with a u32 protocol version,
  and the client must speak exactly that version (32 today) or stop.
- Every packet starts with a 15-byte little-endian header: u8 type (request, reply, notification, server info), u16
  command, u32 total size, u64 packet id.
- A reply's first payload byte is the error code. Strings are NUL-terminated.
- Synchronous replies time out after 10 s.

**Account commands** (payloads are strings in this order):

| Command | Sends | Reply and errors |
|---|---|---|
| Create | username, password key, online name, avatar URL, email | ok, or the username or email is taken, invalid, etc. RPCS3 sends the username as the online name, and its default avatar. |
| Login | username, password key, email token | ok plus online name, avatar URL, user id, friends, friend requests both ways, block list. Errors: invalid username, invalid password, invalid token, already logged in. |
| SendToken | username, password key | sends the email token again. Once every 24 hours ("too soon"). A server without email verification answers "invalid". |
| SendResetToken | username, email | emails a reset token |
| ResetPassword | username, reset token, new password key | ok |
| Delete | (logged in) | deletes the account |

**Rules the dialogs check before sending.**
- Username: 3-16 characters, letters, digits, `-` and `_`.
- Email: a simple pattern.
- Token: exactly 16 characters, 0-9 and A-Z.
- The password and email are typed twice.

**The password key.** PBKDF2-HMAC-SHA3-256, 200,000 iterations, a fixed ASCII salt (the string in
`rpcn_settings_dialog.cpp`), 32 bytes out, as 64 upper-case hex characters.
- Windows' CNG has SHA3 only on recent Windows 11, so we'd carry a small SHA3 (Keccak) implementation with a
  permissive licence and test it against the standard vectors.
- 200,000 iterations take a fraction of a second; run them off the UI thread.

**After login** the same connection carries the NP work: Matching2, signalling info, friends, presence and scores
(docs/69 section 2). The client also pings UDP port 3657 from the P2P socket every 5 s. The ping holds the user id
and the local IPv4 address, so RPCN learns the public address and port peers should use. The local address matters
when two players share one router, as two copies on one PC do (section 4).

## 2. What RPCN allows a third-party client

- **The licence.** The server is AGPL-3.0: anyone may run their own RPCN, with source. That doesn't grant use of
  `np.rpcs3.net`.
- **Terms.** None published for clients, as far as the RPCN repository, the RPCS3 site and the wiki show. RPCN's
  accounts exist for RPCS3 users; the email verification and the one-session rule are the only gates.
- **Identification.** No client name or build string is sent; the protocol version is the only check. So a
  compatible client is indistinguishable from RPCS3. That's exactly why it needs asking rather than assuming.
- **Practical risk.** A protocol bump (version 33) breaks any client that doesn't follow it the same day. Following
  it is ongoing work.
- **Alternatives if they decline:**
  - our own RPCN instance (AGPL server, run by someone), which loses the RPCS3 players;
  - docs/46's psnr route, which loses them as well.

  Neither reaches the people online today.

## 3. Storing the credentials

| What | Where | How |
|---|---|---|
| Server host | settings.ini (`DES_RPCN_HOST`, default `np.rpcs3.net`) | plain |
| Username | `rpcn.dat` in the data folder | plain inside the encrypted blob |
| Password key (not the password) | `rpcn.dat` | DPAPI (`CryptProtectData`, current user): another Windows user, or a copy of the file on another PC, can't read it |
| Email token | `rpcn.dat` | DPAPI |
| Email address | not stored | only typed for Create or a reset |

- The data folder is `%LOCALAPPDATA%`'s app folder, or `data\` beside a portable exe, or `DES_DATA_DIR`. It's never
  inside the repository; test copies' folders are under `work/tmp`, which git ignores.
- **Never in logs:** the client logs "signed in as <username>" and error names, never the key, token or email. The
  crash and issue reports already skip the data folder's files; `rpcn.dat` is added to the report's deny list
  explicitly, with a test.
- **Never in settings.ini**, so pasting your settings into an issue leaks nothing.
- A "Sign out" button deletes `rpcn.dat`. "Delete account" also asks RPCN to delete the account, after a
  confirmation.
- The password key is as good as the password for RPCN, because it is what logs in. DPAPI protects the file at rest.
- **TLS without certificate checks**, as RPCS3 does it, would let anyone on the path read the key. We should check
  the server's certificate: pin RPCN's current certificate or public key, and offer a settings switch to accept a
  self-hosted server's certificate. Decision 3 asks whether to match RPCS3 or check.

## 4. Two copies, two accounts

- `tools/des/duo.sh` gives each copy its own `DES_DATA_DIR` (`$DUO_DIR/a/data`, `$DUO_DIR/b/data`), so each has its
  own `rpcn.dat` and settings.ini. Sign each copy in with its own account from its own Network tab.
- **RPCN allows one session per account** ("already logged in"), so two copies need two accounts, each with its
  own email address. That's yours to make.
- The P2P ports differ (3658 and 3659), so the copies' UDP sockets, and RPCN's view of them, stay apart.
- Both copies share one public IP. RPCN's pings carry each copy's local address, which lets peers behind the same
  router reach each other (inferred from the ping contents; checked when the client exists). If not, NAT hairpinning
  on your router decides.

## 5. Where it goes in our UI

**Settings > Network**, below Online and the server (docs/69), an **RPCN account** section:

| State | Shows | Buttons |
|---|---|---|
| Not signed in | "Co-op and invasions need a free RPCN account (the same one RPCS3 uses)." | Sign in, Create account |
| Created, not verified | "Enter the 16-character code RPCN emailed to <masked email>." | a token field, Verify, Resend code (greyed for 24 h after a send) |
| Signed in | "Signed in as <username>." plus the online status | Sign out, Change password, Delete account |
| Error | One sentence: "That username is taken", "Wrong password", "RPCN is unreachable" | Retry |

- **Sign in** asks for the username and password.
- **Create account** asks for the username, the password twice and the email twice, then shows the token step.
- **Change password** is SendResetToken, then the reset token plus the new password twice.
- Text fields use the existing `StringButton`, with masking added for passwords.
- Every RPCN call runs on a worker thread, and the window shows a spinner. The 200,000-iteration key derivation
  runs there too.

**First run (docs/52).** An optional **Online account** step between Shaders and Done:
- **Skip (later in Settings > Network)** is the default button.
- **Sign in** and **Create account** open the same screens as Settings.
- One line explains what the account is for, and that RPCN is a community service run by RPCS3's developers.
- Skipping keeps online play for messages and bloodstains, which need no account (docs/69 step 1).

**Code split.**
- The RPCN client (connection, commands, key derivation, storage) lives in ps3recomp, because NP needs it.
- The port's UI calls a small C API:
  - `rpcn_account_create`, `rpcn_account_login`, `rpcn_account_resend_token`, `rpcn_account_reset_request`,
    `rpcn_account_reset`, `rpcn_account_sign_out`;
  - each async, with a callback carrying an error code.
- TLS uses Windows' SChannel, so the player installs nothing (the no-new-dependencies rule from the patcher
  notes). Linux and macOS use the system TLS later.

## 6. Effort

| Piece | Days |
|---|---|
| SHA3 and PBKDF2, with test vectors | 0.5 |
| TLS connection (SChannel), header, ServerInfo version check, request/reply plumbing | 2-3 |
| The account commands and their errors | 1-2 |
| `rpcn.dat` with DPAPI, report deny list, tests | 1 |
| The Network tab section and the first-run step | 2-3 |
| Login on boot feeding NP (online name, user id), UDP 3657 ping | 1-2 |
| **Total for accounts and sign-in** | **8-12** |

Matching2, signalling and the P2P framing come on top (docs/69 section 4, part of docs/46's 20-40 days).

## 7. Decisions for you

1. **Ask RPCN's maintainers** (RipleyTom; RPCS3's Discord or the rpcn repository) whether a third-party client may
   use `np.rpcs3.net`, before any client code. Recommended: yes, ask first; I can draft the message.
2. **Accounts for testing:** two RPCN accounts, one per copy, made by you (two email addresses). Not needed until
   the client exists.
3. **TLS:** check RPCN's certificate (recommended), or match RPCS3 and skip the check?
4. **First-run step:** optional and skippable as above, or Settings only?
