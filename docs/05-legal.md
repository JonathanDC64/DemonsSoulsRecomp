# 05 — Legal / Clean-Room Operating Rules

This project must never distribute copyrighted material. These rules are non-negotiable and apply to every commit, push, and shared artifact.

## The model (same as Zelda 64 Recompiled, Unleashed Recompiled)

Ship **only our own code and tooling**. The user supplies their own legally-owned game files locally. Nothing game-derived leaves the machine.

## Hard rules

1. **Never commit or distribute:**
   - The decrypted `EBOOT.ELF` or any PS3 executable/PRX.
   - PS3 decryption keys (lv0/metldr/etc.).
   - Game assets (`.flver`, `.tpf`, `.hkx`, `.dcx`, `.param`, audio, video, …).
   - Generated C/C++ that is a transliteration of the game's machine code.
   - Sony system libraries (e.g. a decrypted `libsre.prx`).
   All of the above live under `work/` and are gitignored.

2. **Do commit / share freely:**
   - Our patches to ps3recomp (MIT) — upstream general fixes.
   - Our own original code: build scripts, analysis tooling, renderer/HLE/SPU-fallback *implementations we write* (not transliterated game code).
   - Documentation, findings, configs that contain no copyrighted data.

3. **SPU fallbacks & shader translations** we author are our own code and are shareable — provided they don't embed extracted game data or transliterated guest code.

## Residual risks (acknowledged, not eliminated)

- This is **mitigation, not immunity.** No court has ruled static recompilation to be fair use.
- **Trademark:** "Demon's Souls" and logos are Sony/FromSoftware marks — avoid using them in any public project/release name; use a neutral codename if this is ever published.
- **Anti-circumvention (DMCA §1201):** defeating SELF encryption can carry exposure independent of asset distribution. We only decrypt the user's own copy, locally, for interoperability.
- Sony is a more litigious rights-holder than those the existing recomp projects faced. Keep anything public asset-free, name-neutral, and tooling-only.

## Practical checklist before any push

- [ ] No files from `work/` staged.
- [ ] No keys, no `.elf`/`.self`/`.prx`, no asset extensions.
- [ ] Generated transliterated C is excluded.
- [ ] Repo/release name is codename, not the trademark.
