# 112. The game's own models in the UI: the sword cursor and the crystal lizard

Date: 2026-10-27. Status: first version, for the user to try.
- While a menu is open, the mouse cursor is the **Large Sword of Moonlight**: the game's own model, drawn live in 3D,
  with its translucent blade and a soft blue glow about it. Its tip is the cursor's point.
- The **crystal lizard** dances in the bottom right corner while a menu is open, turning round slowly (once in
  12 s), and it twinkles: its gems flash and star glints come and go on it. That place is for now. In a document it
  is one line of RCSS (section 5).
- Settings > Interface turns each off: **Sword cursor** and **Crystal lizard**, both on by default, at once.
- Nothing of the game is in the program or in this repository. Both models are read from the player's own copy of
  the game, the first time each is drawn.

## 1. What shows, and the knobs

The two rows are settings with the knobs' names, so a knob given at start wins over its row (the row shows it as
set from outside).

| Knob | Default | What it does |
|---|---|---|
| `DES_CURSOR3D` | `1` | `0`: the system's arrow, as before. Its row is Sword cursor. |
| `DES_CURSOR_SIZE` | `64` | The sword's length, in dp (the UI's units: pixels at 96 DPI, times the UI scale). |
| `DES_CURSOR_SPIN` | `0` | A slow turn about the blade, in degrees a second (`20` is one turn in 18 s). |
| `DES_CURSOR_GLOW` | `1` | `0`: no glow about the blade (section 4.1). |
| `DES_UI_LIZARD` | `1` | The lizard, dancing, in the bottom right corner while a document is up; `0`: none. Its row is Crystal lizard. |
| `DES_UI_LIZARD_SIZE` | `160` | Its box, in dp. |
| `DES_UI_LIZARD_SPIN` | `30` | The lizard turning round as it dances, in degrees a second (`30` is a turn in 12 s; `0` keeps it facing the front left). |
| `DES_UI_LIZARD_ANIM` | `a00_3301` | Another of its animations, by name (`a00_0000` is its idle). |
| `DES_UI_LIZARD_TWINKLE` | `1` | `0`: no twinkle, neither the gems' flashes nor the star glints (section 4.1). |

The sword shows where RmlUi would show the arrow or the hand: over the documents, while one is up, and the mouse is
over the window. The text cursor, the HUD editor's move and resize cursors, and the "unavailable" one stay the
system's. During mouse look the cursor is hidden as before (no document is up then).

If the game's files are not there, or a model does not read, the log says so once
(`[ui] model moonlight: ...`), and the system's arrow stays.

## 2. Where the models come from

From `$PS3_VFS_ROOT/PS3_GAME/USRDIR`, which every run sets to the game's folder:

| Model | Files |
|---|---|
| The sword (`moonlight`) | `parts/wp_a_0258.partsbnd`: `WP_A_0258.flver`, `WP_A_0258.tpf` |
| The lizard (`lizard`) | `chr/c3110/c3110.chrbnd` (`c3110.flver`, `c3110.tpf`), `chr/c3110/c3110.anibnd` (`Skeleton.hkx`, `a00_*.hkx`) |

They are read once and kept in memory.

## 3. The formats (`port/ui/src/souls_model.cpp`)

All are big-endian, as the PS3 wrote them, and read from scratch, with no library.
- **BND3**: an archive. The entry table starts at 0x20; the format byte says which fields each entry has.
- **FLVER0**: the model.
  - Its header, then the dummies (0x40 each), the materials (0x20), the bones (0x80) and the meshes (0x64).
  - A material names its MTD (`*_Alp` is alpha blended) and its textures by kind (`g_Diffuse`, `g_Specular`).
    It also holds the vertex layouts.
  - A mesh has a triangle strip (16-bit, `0xFFFF` restarts) and its vertices, as the layout says:
    - position: 3 floats;
    - normal: 4 bytes around 127;
    - texture coordinates: shorts over 1024;
    - bone weights: shorts over 32767;
    - bone indices: bytes into the mesh's own table of 28 bones.
- **TPF**: the textures, DXT1, DXT3 or DXT5 blocks with their mips. These are the same bytes as BC1, BC2 and BC3 on
  a PC, so they go to the GPU as they are.
- **Havok 5.5 packfiles** (`.hkx`): sections, with pointers found through their fixup tables.
  - The skeleton is an `hkaSkeleton`: the parents, the bone names and the reference pose.
  - An animation comes with its `hkaAnimationBinding`, which says which bone each track moves. It is one of two
    kinds:
    - an `hkaInterleavedSkeletalAnimation`: every frame's transform for every track, as they are (3 of the lizard's
      17);
    - an `hkaWaveletSkeletalAnimation`: compressed, as the other 14 are, the dance among them (section 3.1).

### 3.1 The wavelet animations

The decoder was written again from the game's own code, with no Havok source.
- Where in the game:
  - the sampler is `0x010241F8` (vtable slot 5 of the class);
  - the transforms are put together at `0x0101B980`;
  - the moving values are counted at `0x0101BD70`;
  - a block is sized, unpacked, dequantized and inverse-transformed at `0x013530E0`, `0x000106C0`, `0x01352E48`
    and `0x000102E0`.
- The object's fields are big-endian. Its data buffer, and the bit stream in it, are little-endian.
- Each track has a 16-bit mask:
  - bits 0-1, 2-3 and 4-5 give the translation's, rotation's and scale's kind (2: none);
  - bits 8-6, 12-9 and 15-13 say which of their x, y, z (and w) move;
  - the others are constants, stored once.
- The moving values come in blocks of 8 frames. For each value and block:
  - first a mask of the coefficients that are the quantized zero;
  - then the other coefficients, at the value's bit width;
  - dequantized as `scale * 2^-bits * (q + 0.5) + offset`;
  - then the game's 8-point inverse wavelet (LeGall 5/3) gives the block's frames.
- A stored rotation w of +-2 means "make w from x, y and z, with this sign".
- The reader decodes every pose once, when the model loads. `pose()` then blends the two poses around the time and
  renormalizes the rotation.
- Checked against a separate decoder's test vectors for `a00_0000`:
  - at the poses, equal to within 5e-7;
  - between poses, only w differs, by up to 9e-5. The game rebuilds w after blending, the reader before; this cannot
    be seen.

**The dance is `a00_3301`:** the lizard stands up on its hind legs and sways, a 3.0 s loop of 91 poses. Its idle,
`a00_0000`, is 2.0 s of breathing.

- **Skinning** is done on the CPU, each frame. A bone's matrix is its posed world transform times the inverse of its
  rest one. The FLVER's bones are matched to the skeleton's by name.

## 4. How they are drawn (`port/ui/src/render_d3d12.cpp`)

The UI's D3D12 renderer has a model shader beside RmlUi's.
- It uses the same root signature: the constants, the diffuse texture (t0) and the specular one (t1).
- The light comes from the upper left. Each texel is lit with an ambient part plus Lambert, and the specular
  texture's colour adds a highlight.
- The layers' shared depth-stencil buffer already had a depth part, unused. The models use it: the depth is cleared
  in the square the model can reach, then the opaque meshes are drawn with the depth test.
- The blade (an `_Alp` material) comes last.
  - It is drawn without writing depth, premultiplied, with only its faces toward the viewer, so it reads as one
    sheet of glass.
  - Its texture's alpha is the translucency, and it is tinted a little blue.
- The projection is orthographic. The cursor places the sword's tip at the pointer; the lizard is fitted into its
  box, around every frame of its animation, so it stays in place as it moves.
- A model that spins turns about the upright axis through the middle of its box. Fitted into a box, it is fitted to
  the upright cylinder around its box, which looks the same at every turn, so it keeps its size as it turns.
- The view and the light are the ones the feasibility previews used (`ui::models::look`).

Only the D3D12 renderer draws models. With `DES_UI_RENDER=d3d11` the system's arrow stays.

### 4.1 The glow and the twinkle

In the game both are effects of their own (the sword's is attached at its dummy 206), not parts of the models, and
their files are not read here. These are stand-ins, drawn with the model shader. Their numbers are in
`ui::models::look`.
- **The sword's glow**, as the previews had it:
  - the blade's alpha, in a pale blue (0.55, 0.78, 1.0), at 0.85, is drawn into a postprocess target;
  - it is blurred there with RmlUi's blur, sigma 3.5% of the sword's length (2 px or so at 64 dp);
  - it is put on the layer under the sword, so it shows round the blade and a little through it.
- **The lizard's twinkle**:
  - **Its gems flash.** A gem is a texel more blue, green or violet than red; its skin is grey, brown and pink.
    The texture is cut into cells (64 by 32), each on a clock of its own (1.5 to 4 s), and at about 4 turns in
    10 a cell's gems flash white for an instant.
  - **Star glints.** Four of them, each on its own clock (1.1 s, then 0.37 s longer for each next one), shine for
    half of it. They are four thin rays over a bright core, 22% of the lizard's length at their brightest, and
    they grow and shrink as they brighten and fade.
  - Each turn a glint goes to another gem: one of 64 vertices on gems, spread over its body when it loads.
  - A glint sits where its gem is in the pose, a little in front of it, so the lizard's depth hides those behind
    it. None shows on a gem turned well away. The dance keeps its back, where most gems are, away from its front
    left, so the glints show most while its turn brings its back round (with `DES_UI_LIZARD_SPIN=0`, on its tail and
    at its outline only).

## 5. The lizard in a document

Any element can show it:

```css
.lizard { width: 160dp; height: 160dp; decorator: shader("model:lizard"); }
```

- RmlUi's `shader` decorator passes its value to the renderer, which draws the model fitted into the element's
  box. It follows the element's transform and opacity, and its clip.
- `model:moonlight` works too.

Where it goes in the UI is still to be decided. Until then host.cpp draws it in a corner (`DES_UI_LIZARD`).

## 6. The cursor: how the system's is hidden

- **Windows:** `port/ui/src/mouse.cpp` answers `WM_SETCURSOR` with no cursor while the sword is drawn. When the
  sword comes or goes, the window's thread sets the cursor again at once, not at the next move.
- **Linux:** `port/ui/src/mouse_sdl.cpp` calls `SDL_ShowCursor`.
- The sword is drawn with the frame, so it is a frame behind the hardware cursor. While a document is up the UI
  repaints at the display's refresh rate, so this is one display refresh.

## 7. Not done yet

- The game's own effects (FFX) for the sword's moonlight and the lizard's twinkle. Section 4.1 draws stand-ins.
- A place for the lizard in the menus.
