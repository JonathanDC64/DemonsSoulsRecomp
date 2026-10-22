# Cutscene camera: blending through an Euler flip

Date: 2026-10-19. Status: written statically, then built and run once the same evening: the run confirmed the cause and the fix (see "Run result"). The patch is against the port's master (53a047a). It merged as port #84.

Line numbers like `des_options.cpp:1166` are for `port/des_options.cpp` on master. Labels as in docs/40:
- **Read**: seen in the EBOOT bytes, the lifted code or our source.
- **Inferred**: still needs a run to check.

## What and why

- With `DES_CUTFPS=1`, `des_cut_cam` blends the cutscene camera's 30 Hz keys (`des_options.cpp:1130-1236`). Its guard keeps the nearest key when one step moves the camera more than 2 m, or turns any angle more than 0.5 rad (`des_options.cpp:1166-1175`).
- docs/40 found one guard hit in the arrival cutscene, at 120 and at 60: t = 13.3 s, a 0.01 m move with a 3.14 rad turn (`docs/40-cutscenes-unlocked-fps.md:344`). A half turn in 1/30 s without moving is not a likely camera move. The likely cause is that the track writes the same rotation with the other set of Euler angles: a flip (inferred).
- Keeping the nearest key there stops the camera for one 1/30 s step while the characters keep moving. That is a hitch at 60 and 120. At 30 the game shows key k and then key k+1, which describe the same rotation, so nothing shows.
- **The fix.** At such a step, work out the later key's other set of angles. If that set is within 0.5 rad of the earlier key on every axis, blend to it. If not, keep the nearest key, as today (a real cut).
- **2 pi wraps per axis** were already handled. `angle_delta` (`des_options.cpp:1103-1108`) takes each difference the short way round, in the guard (`des_options.cpp:1169`) and in the blend (`des_options.cpp:1225`). The patch does not change that.
- **Defaults do not change.** Without `DES_CUTFPS` the hooks return at once (`des_options.cpp:1158`), and `DES_CUTFPS` keeps its default and meaning (`des_options.cpp:236-243`). Under `DES_CUTFPS=1` the new blend is on. A new live knob, `DES_CUTEULER=0`, puts back today's hold at a flip. It follows the file's fix knobs such as `DES_CAMFIX` (`des_options.cpp:852-853`): default 1, and 0 turns it off.
- Why on by default under `DES_CUTFPS=1`: the hold is a hitch the port adds. The game at 30 has none there. So the blend brings `DES_CUTFPS=1` closer to how the game looks at 30.
- **No hook site changes.** `port/insn_hooks.txt:25-32` stays as it is, so this needs a port rebuild only, not a re-lift.

## The derivation

### The rotation order (read, in the EBOOT)

docs/40 says `0x000562F8` builds the camera matrix from a scale, Euler angles (through `0x00054D48`) and a translation (`docs/40-cutscenes-unlocked-fps.md:62`). Reading the code:

- `0x005F71D0` copies the rotation key's three floats from the table into the vector in order: +0, +4 and +8 go to elements 0, 1 and 2 (the copy at `0x005F7598`). It wraps them after the load at `0x005F7394` and passes the result in v3 to `0x000562F8` (call at `0x005F7448`). So the key's (x, y, z) are the vector's elements 0, 1 and 2, the same order `cut_keys` reads (`des_options.cpp:1121-1124`).
- `0x000562F8` passes the angles to `0x00054D48` in v2 (call at `0x0005638C`), with an output matrix on its stack.
- `0x00054D48` takes the angles one at a time: element 0, then element 2, then element 1. For each it calls `0x00054148`, a vector sine and cosine. That function reduces the angle by quarter turns, works out an odd polynomial (the sine) and an even one (the cosine), and stores the sine through its first pointer (r5) and the cosine through its second (r6). As a cross-check, zero angles then give the identity matrix.
- From each angle it builds one axis matrix out of the sine, the cosine, the negated sine and constant unit rows, placed with lane masks. The constants were read too: unit rows (1, 0, 0, 0), (0, 1, 0, 0), (0, 0, 1, 0), (0, 0, 0, 1), a zero row, and one mask per lane. The matrices' rows are:
  - element 0 (x): (1, 0, 0), (0, c, s), (0, -s, c). A turn about X.
  - element 2 (z): (c, s, 0), (-s, c, 0), (0, 0, 1). A turn about Z.
  - element 1 (y): (c, 0, -s), (0, 1, 0), (s, 0, c). A turn about Y.
- It multiplies them in that order: the X matrix times the Z matrix, then that times the Y matrix. Each result row is a sum of the right-hand matrix's rows.
- `0x000562F8` then forms scale times rotation times translation, with the translation in the bottom row. That is the row-vector convention: a point p becomes p M.

So **R = Rx(x) Rz(z) Ry(y)**, with row vectors. A point turns about X first, then Z, then Y (fixed axes). Z is the middle turn.

### The equivalent set

For three different axes, the first and the last angle move by pi, and the middle angle becomes pi minus itself. Here the middle axis is Z:

**(x, y, z) and (x + pi, y + pi, pi - z) give the same matrix.**

Why:
- Rx(x + pi) = Rx(x) Rx(pi) and Ry(y + pi) = Ry(pi) Ry(y). So the right-hand set gives Rx(x) [Rx(pi) Rz(pi - z) Ry(pi)] Ry(y). It is enough to show the bracket is Rz(z).
- Rx(pi) = diag(1, -1, -1) and Ry(pi) = diag(-1, 1, -1).
- Rz(pi - z) has rows (-cos z, sin z, 0), (-sin z, -cos z, 0), (0, 0, 1).
- diag(1, -1, -1) on the left negates rows 2 and 3: (-cos z, sin z, 0), (sin z, cos z, 0), (0, 0, -1).
- diag(-1, 1, -1) on the right negates columns 1 and 3: (cos z, sin z, 0), (-sin z, cos z, 0), (0, 0, 1). That is Rz(z).

For X then Y then Z the middle axis is Y, which gives the (x + pi, pi - y, z + pi) form. That form is not an identity for this order.

Away from z = +-pi/2, these two sets (each angle up to 2 pi) are the only ones for a rotation. So "the later key's equivalent set nearest the earlier key" is the key itself or its other set, whichever is closer with each angle taken the short way round.

A numeric check (a scratch script, 10,000 random sets, this order):
- (x + pi, y + pi, pi - z): largest matrix difference 7e-16.
- The X-Y-Z form on this order: off by up to 2.0 in a matrix entry.
- 2 pi on any axis: 2e-15.
- A flip between two keys a small step apart: blending to the other set matches the true in-between rotation within 2e-6 degrees. Blending the raw angles is off by up to 180 degrees.

## The exact edits

All in `port/des_options.cpp`. Master line numbers first; the new ones after the patch in brackets.

1. **Constant** (`:1091-1092`): `kPi` next to `kTwoPi` [1092].
2. **Thread state** (`:1094-1096`): `s_cut_flip` next to `s_cut_blend`, set once a call with it [1095-1099].
3. **Helper** after `angle_delta` (`:1108`): `euler_other(e, c)` returns angle c of the other set, `c == 2 ? pi - e[2] : e[c] + pi` [1113-1121].
4. **Comment** above `des_cut_cam` (`:1147-1149`): describes the flip and the knob [1160-1168].
5. **The decision** in `des_cut_cam` (`:1150-1175`) [1169-1201]:
   - a live knob `euler`: `PS3_KNOB(euler, ps3_env_int("DES_CUTEULER", 1) != 0)`, next to `log`;
   - `s_cut_flip = false` next to `s_cut_blend = false`;
   - `r0` and `r1` start at zero, because the log may now print them for a skipped channel;
   - the loop also works out `turn_e`: the largest angle change from the earlier key to the later key's other set;
   - `flip = euler && turn > 0.5 && turn_e <= 0.5`;
   - `jump` tests `turn_e` instead of `turn` at a flip. A move over 2 m is still a jump;
   - `s_cut_flip = s_cut_blend && flip`.
6. **The log** under `DES_CUTLOG=1` (`:1176-1194`) [1202-1226]: counts flips; prints one line per jump or flip (once per track and key time, as before) with both rotation keys and the later key's other set, wrapped to [-pi, pi]. The summary adds "(Euler flips N)".
7. **The blend** (`:1224-1225`) [1256-1259]: at a flip, the later key's angles are swapped for `euler_other(y, i)` before `angle_delta`. Position and scale are not touched.

With `DES_CUTEULER=0`, `flip` is always false. The guard, the blend and the hold are then exactly as on master; only the log line is longer.

## The patch

Against master (53a047a). `git apply --check` passes on master's tree.

```diff
diff --git a/port/des_options.cpp b/port/des_options.cpp
index 7e4d5f2..4b5ec34 100644
--- a/port/des_options.cpp
+++ b/port/des_options.cpp
@@ -1089,11 +1089,14 @@ constexpr uint32_t kCutCamRot   = 0x005F7394u;   /* lvx v12: earlier r1+0xD0, la
 constexpr uint32_t kCutCamScale = 0x005F743Cu;   /* lvx v2: earlier r1+0x70, later r1+0xA0 */
 constexpr float    kCutMaxMove  = 2.0f;          /* m in one 1/30 s step */
 constexpr float    kCutMaxTurn  = 0.5f;          /* rad, any angle, in one step */
+constexpr float    kPi          = 3.14159265f;
 constexpr float    kTwoPi       = 6.28318531f;
 
 /* The choice for the call of 0x005F71D0 running on this thread, made at
- * kCutCamKeys before its three loads: blend them, or leave them alone. */
+ * kCutCamKeys before its three loads: blend them, or leave them alone; and
+ * whether the rotation blends to the later key's other Euler angles. */
 PPU_THREAD_LOCAL bool s_cut_blend;
+PPU_THREAD_LOCAL bool s_cut_flip;
 
 uint32_t bswap32(uint32_t b)
 {
@@ -1107,6 +1110,16 @@ float angle_delta(float a, float b)
     return d - kTwoPi * std::nearbyint(d / kTwoPi);
 }
 
+/* Angle c of the other Euler angles for the rotation e. 0x00054D48 builds
+ * the camera's rotation from (x, y, z) as Rx(x) Rz(z) Ry(y), row vectors, so
+ * z is the middle turn, and Rx(x + pi) Rz(pi - z) Ry(y + pi) is the same
+ * matrix (docs/62). Angles 2 pi apart need nothing: angle_delta takes the
+ * short way round. */
+float euler_other(const float e[3], int c)
+{
+    return c == 2 ? kPi - e[2] : e[c] + kPi;
+}
+
 /* One channel's two keys, read from the track as 0x005F71D0 reads them. i0
  * and i1 are the keys' indices; the game skips the channel when i0 is -1.
  * The byte at h + zero makes both keys read as zero, and the word at
@@ -1145,52 +1158,71 @@ int cut_keys(uint32_t h, uint32_t zero, int32_t i0, int32_t i1, float k0[3], flo
  * turns it over 0.5 rad, a shot change inside one track. After each of the
  * three loads it then puts the two keys blended by s in the register.
  * Rotations are Euler angles in radians and blend the short way round; the
- * code after the load wraps them again. A register that holds neither key
- * is left alone. DES_CUTLOG=1 counts what it did. Main thread. */
+ * code after the load wraps them again. A step whose angles turn over
+ * 0.5 rad while the later key's other angles for the same rotation
+ * (euler_other) are within 0.5 rad of the earlier key is not a turn: the
+ * track wrote the same rotation the other way (a flip). The rotation then
+ * blends to those other angles; keeping the nearest key there held the
+ * camera for a step (docs/62). DES_CUTEULER=0 keeps the nearest key at a
+ * flip, as before, live. A register that holds neither key is left alone.
+ * DES_CUTLOG=1 counts what it did. Main thread. */
 extern "C" void des_cut_cam(ppu_context* ctx, uint32_t addr)
 {
-    static int log = 0;
+    static int log = 0, euler = 1;
     PS3_KNOB(log, env_on("DES_CUTLOG"));
-    static unsigned n_calls, n_blend, n_apart, n_jump, n_odd, n_neither;
+    PS3_KNOB(euler, ps3_env_int("DES_CUTEULER", 1) != 0);
+    static unsigned n_calls, n_blend, n_flip, n_apart, n_jump, n_odd, n_neither;
     static double widest;   /* the widest gap between keys, frames */
     if (addr == kCutCamKeys) {
         s_cut_blend = false;
+        s_cut_flip = false;
         if (!cutfps_on() || s_fps <= 0) return;
         const double s = ctx->fpr[10], gap = ctx->fpr[11];
         if (!(s > 0.0 && s < 1.0)) return;   /* t on a key: the loads are exact */
         const uint32_t h = (uint32_t)ctx->gpr[7];
-        float p0[3], p1[3], r0[3], r1[3], k0[3], k1[3];
+        float p0[3], p1[3], r0[3] = {}, r1[3] = {}, k0[3], k1[3];
         const int pos   = cut_keys(h, 0x2C, (int32_t)ctx->gpr[6], (int32_t)ctx->gpr[4], p0, p1);
         const int rot   = cut_keys(h, 0x2F, (int32_t)ctx->gpr[29], (int32_t)ctx->gpr[31], r0, r1);
         const int scale = cut_keys(h, 0x32, (int32_t)ctx->gpr[12], (int32_t)ctx->gpr[28], k0, k1);
-        float move = 0.0f, turn = 0.0f;
+        float move = 0.0f, turn = 0.0f, turn_e = 0.0f;   /* turn_e: to the later key's other angles */
         for (int c = 0; c < 3; c++) {
             if (pos > 0) move += (p1[c] - p0[c]) * (p1[c] - p0[c]);
-            if (rot > 0) turn = std::fmax(turn, std::fabs(angle_delta(r0[c], r1[c])));
+            if (rot > 0) {
+                turn   = std::fmax(turn, std::fabs(angle_delta(r0[c], r1[c])));
+                turn_e = std::fmax(turn_e, std::fabs(angle_delta(r0[c], euler_other(r1, c))));
+            }
         }
         move = std::sqrt(move);
+        const bool flip  = euler && turn > kCutMaxTurn && turn_e <= kCutMaxTurn;
         const bool apart = std::fabs(gap * 30.0 - 1.0) > 0.01;
-        const bool jump  = !apart && (move > kCutMaxMove || turn > kCutMaxTurn);
+        const bool jump  = !apart && (move > kCutMaxMove || (flip ? turn_e : turn) > kCutMaxTurn);
         const bool odd   = pos < 0 || rot < 0 || scale < 0;
         s_cut_blend = !apart && !jump && !odd;
+        s_cut_flip  = s_cut_blend && flip;
         if (log) {
             n_calls++;
             if (s_cut_blend) n_blend++;
+            if (s_cut_flip) n_flip++;
             if (apart) { n_apart++; if (gap * 30.0 > widest) widest = gap * 30.0; }
             if (jump) n_jump++;
             if (odd) n_odd++;
-            static uint32_t jump_h;
-            static double jump_t0 = -1.0;
-            if (jump && !(h == jump_h && ctx->fpr[12] == jump_t0)) {   /* f12: the earlier key's time */
-                jump_h = h;
-                jump_t0 = ctx->fpr[12];
-                fprintf(stderr, "[cutlog] t=%.3f s: a jump (move %.2f m, turn %.2f rad), nearest key kept\n",
-                        ctx->fpr[1], move, turn);
+            static uint32_t said_h;
+            static double said_t0 = -1.0;
+            if ((jump || s_cut_flip) && !(h == said_h && ctx->fpr[12] == said_t0)) {   /* f12: the earlier key's time */
+                said_h = h;
+                said_t0 = ctx->fpr[12];
+                float e[3];   /* the later key's other angles, wrapped to [-pi, pi] */
+                for (int c = 0; c < 3; c++) e[c] = angle_delta(0.0f, euler_other(r1, c));
+                fprintf(stderr, "[cutlog] t=%.3f s: %s (move %.2f m, turn %.2f rad, %.2f to the other angles), %s;"
+                        " rotation keys (%.4f %.4f %.4f) to (%.4f %.4f %.4f), other angles (%.4f %.4f %.4f)\n",
+                        ctx->fpr[1], jump ? "a jump" : "an Euler flip", move, turn, turn_e,
+                        jump ? "nearest key kept" : "blended to the other angles",
+                        r0[0], r0[1], r0[2], r1[0], r1[1], r1[2], e[0], e[1], e[2]);
             }
             if (n_calls % 1000 == 0)
-                fprintf(stderr, "[cutlog] calls between keys %u: blended %u, keys apart %u (widest %.1f frames),"
-                        " jumps %u, one index unset %u; loads holding neither key %u\n",
-                        n_calls, n_blend, n_apart, widest, n_jump, n_odd, n_neither);
+                fprintf(stderr, "[cutlog] calls between keys %u: blended %u (Euler flips %u), keys apart %u"
+                        " (widest %.1f frames), jumps %u, one index unset %u; loads holding neither key %u\n",
+                        n_calls, n_blend, n_flip, n_apart, widest, n_jump, n_odd, n_neither);
         }
         return;
     }
@@ -1222,7 +1254,9 @@ extern "C" void des_cut_cam(ppu_context* ctx, uint32_t addr)
         return;
     }
     for (uint32_t i = 0; i < 4; i++) {
-        const float d = i == 3 ? 0.0f : ch == 1 ? angle_delta(x[i], y[i]) : y[i] - x[i];
+        /* At a flip the rotation blends to the later key's other angles. */
+        const float yi = ch == 1 && s_cut_flip && i < 3 ? euler_other(y, (int)i) : y[i];
+        const float d = i == 3 ? 0.0f : ch == 1 ? angle_delta(x[i], yi) : yi - x[i];
         const float f = x[i] + d * (float)s;
         uint32_t fb;
         memcpy(&fb, &f, 4);
```

## What the diagnostic run must show

The diagnostic (`cutlog_rot.patch`, a scratch patch for the lane build) adds both rotation keys to the guard's line:

`[cutlog] t=13.3.. s: a jump (move 0.01 m, turn 3.14 rad), nearest key kept; rotation keys (x0 y0 z0) to (x1 y1 z1)`

**If the cause is a flip in this order**, with each difference wrapped to [-pi, pi]:
- x1 - x0 is near +-pi (off by no more than the step's own turn, a few hundredths of a radian);
- y1 - y0 is near +-pi too;
- z1 + z0 is near pi (wrapped, near +-pi). In other words z1 is about pi - z0;
- the later key's other set, (x1 + pi, y1 + pi, pi - z1) wrapped, is within 0.5 rad of (x0, y0, z0) on every axis. In practice it should be within about 0.05 rad, one 1/30 s camera step.

An example with made-up numbers: (0.3000 -2.0000 0.1000) to (-2.8316 1.1316 3.0416). The guard would log a 3.13 rad turn. The other set of the later key is (0.3100 -2.0100 0.1000), 0.01 rad from the earlier key.

To work out the other set from the diagnostic's six numbers (x0 y0 z0 x1 y1 z1, in the order printed), from any shell:

```
python -c "import math,sys; w=lambda a: a-2*math.pi*round(a/(2*math.pi)); x0,y0,z0,x1,y1,z1=map(float,sys.argv[1:7]); e=(w(x1+math.pi),w(y1+math.pi),w(math.pi-z1)); print('other set %.4f %.4f %.4f, off by %.4f rad' % (e+(max(abs(w(a-b)) for a,b in zip(e,(x0,y0,z0))),)))" 0.3000 -2.0000 0.1000 -2.8316 1.1316 3.0416
```

It prints `other set 0.3100 -2.0100 0.1000, off by 0.0100 rad` for the example. A flip shows as "off by" well under 0.5 rad.

The patched build prints the same with `DES_CUTLOG=1`, and works out the other set itself: `an Euler flip (move 0.01 m, turn 3.14 rad, 0.0x to the other angles), blended to the other angles; rotation keys (...) to (...), other angles (...)`. At 120 the summary should read about `blended 3000 (Euler flips 4), ... jumps 0`, where docs/40 had `2996 blended ... jumps 4`. A step has about four calls at 120 and two at 60 (inferred from docs/40's counts).

**What rules it out.** In each case below the patch does nothing at that step, and the guard holds as today.
- Only one angle differs by about pi and the other two hardly change. That is a real half turn about one axis, such as a reverse shot from the same place. The hold is then the guard working as designed.
- z0 and z1 are near +-pi/2 (within about 0.2 rad), and x and y jump a lot while x - y (at +pi/2) or x + y (at -pi/2) stays about the same. That is a swing near gimbal lock: there the two sets merge and neither is close. That would need a quaternion blend (see Risks).
- x and y differ by pi but z1 is about z0 or about -z0 instead of pi - z0. Then the order is not the one read above: re-read `0x00054D48`.
- The other set is more than 0.5 rad off with none of these patterns. Then look elsewhere, for example at the keys' indices.

## Run result (2026-10-19 evening)

The patch was built into a developer build of master (both repos as of the 18:47 merges, re-lifted; one file compiled), and the arrival cutscene ran twice from the pinned regression save at `DES_FPS=120 DES_CUTFPS=1 DES_CUTLOG=1`. The user's save was identical after each run.

| Run | The line at t = 13.308 s | Calls between keys | Blended | Euler flips | Jumps |
|---|---|---|---|---|---|
| `DES_CUTEULER=0` (the old guard) | `a jump (move 0.01 m, turn 3.14 rad, 0.01 to the other angles), nearest key kept` | 3000 | 2996 | 0 | 4 |
| The fix | `an Euler flip (move 0.01 m, turn 3.14 rad, 0.01 to the other angles), blended to the other angles` | 3000 | 3000 | 4 | 0 |

- Both runs printed the same keys: (-1.6427 1.5704 0.0000) to (1.4980 -1.5616 3.1416), with the later key's other angles (-1.6436 1.5800 0.0000).
- So it is the flip this doc predicted: x and y each differ by about pi, z1 is pi - z0, and the other set is within 0.01 rad of the earlier key, one camera step. None of the "what rules it out" cases applies.
- The four jumps without the fix are the same step seen by four calls between the two keys (at 120 fps, four frames share each 30 Hz step).
- It merged as port #84.

## Risks

- The order comes from reading the disassembly, not from a run. Each step was read (the three axis matrices, the order of the two products, the sine and cosine outputs, the row-vector convention), and a script checked the identity on that order. The diagnostic's numbers are the check in the game.
- If the cause is not a flip, the patch does nothing at that step and the hitch stays. The log line says which case it was.
- A wrong flip cannot swing the camera. A step counts as a flip only when the later key's other set, which is the same rotation as the later key, is within 0.5 rad of the earlier key. Blending to it is then an ordinary small blend.
- Euler blends near gimbal lock (z near +-pi/2) are not exact rotation blends. That is true of today's blend too, and the steps are small. A full answer there is a quaternion slerp of the two keys, turned back into this order's angles. It is more code, and only needed if the diagnostic shows a gimbal swing.
- The blended angles can leave [-pi, pi], for example x0 near pi plus a small step. The game wraps the loaded vector right after the load (`docs/40-cutscenes-unlocked-fps.md:58`), and the matrix is the same either way.
- The angles feed only the matrix. In `0x005F71D0` the wrapped vector goes to `0x000562F8` and nowhere else (read in the lifted function). So the other set near the end of one step and the raw key at the start of the next give the same camera.
- The `[cutlog]` line's text changes. Nothing in `tools/` or `scripts/` on master parses it.
- The diagnostic patch and this patch edit the same log line. Do not stack them. This patch prints everything the diagnostic prints, and the other set as well.
- Cost: three more angle differences once a call, and only under `DES_CUTFPS=1`.

## How to test tomorrow

Build the branch. It is a port rebuild only: `insn_hooks.txt` does not change, so there is no re-lift. Then play the arrival cutscene at 120 with the log on:

```
& "C:\Program Files\Git\bin\bash.exe" -c "tools/des/play.sh DES_SKIP_INTRO=1 PS3_SAIL_MOVIES=0 DES_AUTO_CONTINUE=1 DES_FPS=120 DES_CUTFPS=1 DES_CUTLOG=1"
```

What to look for:
- About 13 s into the arrival cutscene (the log's t is the cut's own clock), the camera used to stop for one 30 Hz step while the characters kept moving: a small hitch. With the fix the pan should stay smooth.
- The log, after the run: `Select-String -Path work/tmp/play.txt -Pattern "cutlog"`. Expect `an Euler flip ... blended to the other angles`, a summary with `(Euler flips N)` above 0 and `jumps 0`, and no `a jump` line at 13.3 s.
- For the other side of an A/B, run the same command with `DES_CUTEULER=0` added. The hitch and the `a jump ... nearest key kept` line should come back, now with both keys and the other set printed.
- The t = 13.3 s step is in the cutscene that the pinned regression save loads into (docs/40, section 5). With another save another cutscene plays; check its log the same way. For the exact case, docs/40's R4 harness works with `DES_CUTLOG=1` added:

```
& "C:\Program Files\Git\bin\bash.exe" -c "DES_SCRATCH=work/tmp/cutfps LC_SETTLE=5 LC_MAX=300 LC_EVERY=1 tools/des/levelcap.sh euler120 work/tmp/cutfps_bursts.txt DES_FPS=120 DES_CUTFPS=1 DES_CUTLOG=1 CELLMARK_DUMP_SKIP=100000000 CELLMARK_DUMP_CHANGED=0 DES_FRAMELOG=work/tmp/cutfps/frames_euler120.txt"
```

## Branch and commit

Branch: `fix/cut-euler-blend` (port), from master.

Commit message:

```
port: the cutscene camera blends through an Euler flip instead of holding a key

With DES_CUTFPS=1, des_cut_cam blends the cutscene camera's 30 Hz keys.
A step whose rotation angles turn more than 0.5 rad keeps the nearest
key, so that real shot changes stay cuts. The arrival cutscene has one
such step at t = 13.3 s: a 0.01 m move with a 3.14 rad turn (docs/40).
That is most likely the same rotation written as the other set of Euler
angles. Above 30 fps the hold shows as a hitch.

0x00054D48 builds the camera's rotation from (x, y, z) as Rx(x) Rz(z)
Ry(y), with row vectors, so Z is the middle turn and
Rx(x + pi) Rz(pi - z) Ry(y + pi) is the same matrix. When the two keys'
angles are more than 0.5 rad apart but the later key's other angles are
within 0.5 rad of the earlier key, the rotation now blends to those other
angles. Otherwise the guard keeps the nearest key, as before. Plain
2 pi wraps per axis were already handled by angle_delta.

DES_CUTEULER=0 (live) keeps the nearest key at a flip, as before.
DES_CUTLOG=1 now prints both rotation keys and the later key's other
angles for each jump or flip, and counts the flips. Without DES_CUTFPS
nothing changes. No hook sites change, so no re-lift is needed. docs/62
has the derivation.

Not built or run yet: written statically on 2026-10-19.
Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
```

Committed on the evening of 2026-10-19 as 796565b, merged as port #84. In the committed message the first paragraph says the cause is confirmed (the other angles are within 0.01 rad of the earlier key), a paragraph gives the run's result, and the usual tested line replaces "Not built or run yet".
