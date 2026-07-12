# HMC ASI Plugins (Hitman: Contracts)

ASI plugins and a bundled ASI loader for **Hitman: Contracts** on modern
systems, especially CrossOver/Rosetta 2 on Apple Silicon Macs. It is the
sibling of the [Hitman 2: Silent Assassin build](https://github.com/rtrydev/h2sa-asi-plugins) and the
[Hitman: Codename 47 build](https://github.com/rtrydev/hc47-asi-plugins). Contracts runs on the same
engine lineage as Hitman 2 and is likewise a **Direct3D 8** title, so the
loader-and-hooks approach is shared.

The repo builds three artifacts:

- `d3d8.dll` — a Direct3D 8 proxy that doubles as the ASI loader. It loads
  every `*.asi` from `scripts/`, forwards the real d3d8 exports to the system
  d3d8, and wraps the D3D8 COM interface so plugins can influence device
  creation and rendering without patching game code.
- `hmc_display.asi` — makes the game **start** under CrossOver (the stock
  exclusive-fullscreen device fails there) and renders it in correct
  widescreen at any resolution. Also fixes the mouse and frame pacing under
  CrossOver, and can run the post-filter effects at full resolution. Bundles
  the profiler: a top-right on-screen overlay showing FPS, frame time, and an
  EIP-sampled CPU-time breakdown (see below).
- `hmc_reduced_x87.asi` — translates the game's x87 float code to SSE2 at
  runtime for a large CrossOver/Rosetta 2 performance win (see below).

The widescreen/startup fix hooks the fixed Direct3D 8 COM ABI, so it has **no
game-build byte offsets** to break. The x87 plugin does patch game code, but
every site is byte-checked against the retail build (PE timestamp + image
size + per-function entry bytes) and it declines to patch a module it does
not recognize.

## How Contracts differs from Hitman 2

Hitman 2 is split across DLLs: a packed `hitman2.exe` loads `RenderD3D.dll`
at run time, and only that DLL touches Direct3D. **Contracts is a single
monolithic, unpacked executable** — renderer, sound and script engine are all
statically linked into `HitmanContracts.exe`, which imports d3d8 directly.
Three consequences run through the whole suite:

1. **The d3d8.dll proxy attaches at process start** (a static import), before
   the game creates its device.
2. **The x87 translation reads `HitmanContracts.exe` straight from disk.**
   There is no memory-dump step (Hitman 2 needed one because its exe ships
   packed), so this repo has no dumper.
3. **The profiler sees one game module.** There is no separate renderer DLL
   to bucket apart from game logic, so the CPU split is X87 (translated) /
   GAME (everything untranslated in the exe) / REST.

Contracts also shares Hitman 2's exact startup failure — *"Unable to create
device. Try changing resolution or color depth"* — so the same windowed/
borderless startup fix applies unchanged.

## Build & Install

### mac

```sh
brew install mingw-w64
pip3 install capstone pefile   # only needed for the x87 plugin's blob
python3 tools/translate.py     # writes dist/HitmanContracts.exe.x87
(cd runtime && make)           # writes dist/d3d8.dll + the .asi plugins
./install.sh                   # copies them into the game + configures the bottle
./install.sh -u                # uninstall (leaves your plugin .ini files)
```

The `translate.py` step is only for `hmc_reduced_x87.asi`; the loader and the
display plugin build without it. `install.sh` installs the
x87 plugin only if both `dist/hmc_reduced_x87.asi` and
`dist/HitmanContracts.exe.x87` exist.

By default `install.sh` targets:

```text
mac:     $HOME/Library/Application Support/CrossOver/Bottles/Steam/drive_c/Program Files (x86)/Steam/steamapps/common/Hitman Contracts
Windows: C:\Program Files (x86)\Steam\steamapps\common\Hitman Contracts
```

Override with `HMC_GAME_DIR=/path/to/game ./install.sh`.

On mac the installer also configures the bottle:

- `d3d8=native,builtin` DLL override, so the game-directory proxy wins over
  the builtin d3d8.
- `Mac Driver\CaptureDisplaysForFullscreen=y`, so winemac captures the
  display for a borderless-fullscreen window — which is what hides the macOS
  menu bar and stops the host (Mac) cursor showing through near the Dock.
- ensures `HitmanContracts.ini` has a `Resolution WxH` line (the stock ini
  has none). If absent, `1920x1080` is added; a small 4:3 placeholder is
  bumped. Backup: `HitmanContracts.ini.bak`; override with
  `HMC_RESOLUTION=WxH`.

### Windows

The same sources build with MSYS2's 32-bit mingw-w64 gcc:

```powershell
winget install MSYS2.MSYS2
C:\msys64\usr\bin\pacman.exe -S --noconfirm mingw-w64-i686-gcc make
.\build.ps1 -Install
```

On real Windows the app-directory `d3d8.dll` is used automatically, so no DLL
override is needed.

### Launch through Steam

Launch the game the normal way (through the Steam client, appid `247430`),
not by running `HitmanContracts.exe` directly, so the game's Steam check
passes.

## Plugin: Display — widescreen + startup fix + profiler (`hmc_display.asi`)

Two problems on a modern Mac, both handled at the D3D8 boundary:

### Startup — "Unable to create device"

In its default configuration the game asks D3D8 for an **exclusive
fullscreen** device at the configured resolution and colour depth. Under the
CrossOver D3DMetal/wined3d stack that `CreateDevice` fails, and the game then
shows its own fatal box — *"Unable to create device. Try changing resolution
or color depth"*. The fix is the same as the Hitman 2 build: **don't use
exclusive fullscreen.** The plugin rewrites the presentation parameters to
windowed (which uses the desktop format and always creates) and, by default
under Wine, strips the window to a borderless popup filling the desktop so it
still looks fullscreen.

### Widescreen — resolution and aspect

The resolution comes from the `Resolution WxH` line in `HitmanContracts.ini`
(the installer adds one — the stock ini has none). The plugin **pins the
backbuffer to the ini resolution** — the same value the engine lays its
viewport, HUD and mouse mapping out against — and applies the standard
**Hor+** projection correction: the horizontal field of view widens to the
true aspect while the vertical FOV is preserved. Orthographic (2D/HUD)
projections are detected and left untouched, so menus and HUD are not
distorted.

Two runtime clamps make arbitrary resolutions robust even where the engine's
own mode selection misbehaves, entirely at the D3D8 ABI (no byte offsets): an
out-of-range viewport is clamped back to the backbuffer, and a collapsed
projection is rebuilt from the real aspect. The engine's 4:3 resolution-snap
quirk (inherited from Hitman 2) is therefore neutralized at runtime rather
than by a byte patch.

### Fullscreen vs. borderless

By default the game runs **borderless** — a frameless window filling the
screen. This always creates, survives alt-tab, and needs no display mode
switch. **Exclusive fullscreen** (`Fullscreen=1`) is **real-Windows only**
(and only at an enumerated display mode); on Wine/CrossOver it is broken —
winemac drives the captured display at a 2× Retina backing scale, so a
fullscreen surface renders larger than the physical panel — and is treated as
borderless-fullscreen, which fills the screen correctly.

### Locked aspect ratio — `PreserveAspect` (default on)

The backbuffer has the `HitmanContracts.ini` resolution's aspect, and when the
screen's differs (a 16:9 `Resolution` on a MacBook's ~16:10 panel, say),
stretching it across a desktop-covering window distorts the image vertically.
With `PreserveAspect=1` (the default) the game window is instead sized to the
**largest centred rect of the backbuffer's aspect** that fits the screen, and
a full-screen **black backdrop window** beneath it supplies the letterbox
(top/bottom) or pillarbox (left/right) bars — nothing is cropped or
stretched. Set `PreserveAspect=0` for the old stretch-to-fill behaviour, or
set `Resolution` to your display's own resolution for a bar-free, pixel-exact
fill.

Two designs that do *not* work on this stack, for the record (both were tried
here or in the H2SA sibling): presenting into a sub-rect of a full-screen
window puts the Steam overlay's corner popups in the bar region, where the
periodic black repaint fights them (continuous blinking); and any per-frame /
per-tick backdrop refill or `SetWindowPos` re-slot flushes a full-screen
winemac surface on the Cocoa main thread — where presents also run — causing
a rhythmic in-game stutter. So the window is the image, the backdrop is
painted only when its update region reports actual dirt, re-slotted only when
it is not below the game window at all, and the fullscreen-*sized* backdrop
keeps winemac's fullscreen treatment (hidden menu bar, raised level, display
capture) engaged even though the game window no longer covers the screen.

### Modern in-game resolution list — `ModernModes` (default on)

Contracts builds its video-options resolution list straight from the D3D8
mode enumeration (`GetAdapterModeCount` / `EnumAdapterModes`, filtered to
32-bit modes and deduped by size). Under CrossOver that enumeration returns
winemac's **scaled Mac desktop modes** (1147×745, 1352×878, 1512×982, …) —
nothing a game would normally run at — and on a Retina panel those logical
"points" sizes also understate the device: a 1512×982 14" MacBook really
drives 3024×1964 pixels.

With `ModernModes=1` the loader serves the game a curated list instead: the
16:9 and 16:10 resolutions games typically offer (1280×720 … 3840×2160),
**limited to what the device supports** — capped at the largest real
enumerated display mode, raised under Wine to 2× the logical desktop to
undo the hidden Retina scale — plus the current `HitmanContracts.ini`
resolution so the active setting stays selectable. Picking an entry in-game
is honoured at the device reset (and the `Resolution` line is re-read on
every device create/reset, so an edited ini takes effect on the next launch
without fighting the boot-time value). The borderless/letterbox presenter
handles any offered size on any screen; on real Windows the exclusive-
fullscreen path still validates against real display modes and falls back to
borderless for sizes the display cannot mode-switch to, so every entry is
operational there too. `ModernModes=0` passes the real enumeration through.

### Frame-rate cap

The engine advances its simulation from the measured frame time, so an
uncapped modern GPU makes physics, camera and scripted timing run wild. The
loader wraps `Present` and the plugin paces each frame to hold `FpsCap`
(default **60**). Set `FpsCap=0` to disable. Applies on every platform.

The limiter also fixes a pacing bug inherited from the Hitman 2 build: a
frame that ran *longer* than the cap period used to get a full extra period
of sleep piled on — roughly halving the frame rate of any scene that dips
below the cap. Hitman 2 never dips below 60 so it never surfaced there;
Contracts' heavy scenes do. When behind schedule the limiter now presents
immediately instead of sleeping.

### Mouse buttons — `ForceWinMouse` (automatic on CrossOver)

Contracts defaults to reading the mouse through **DirectInput**, and winemac
delivers DirectInput mouse *motion* but not *button* state — so the cursor
moves and menu items highlight on hover, but **clicks and firing do
nothing**, on every device. The game has a second, working (Windows) mouse
path, and the plugin switches it on at runtime — the mouse just works, with
no game-ini change. `ForceWinMouse=-1` (the default) applies this under Wine
only; on real Windows the DirectInput path already reports buttons, so it is
left alone.

### Mouse-look "edge wall" — `MouseClipFix` (automatic on CrossOver)

Turning the camera, the view would stop dead against an invisible line —
usually toward the **right or down** — letting the mouse only travel back or
slide along the other axis. The same bug is in Hitman 2: it is the engine's,
not this build's.

For mouse-look the engine captures the pointer by clipping the OS cursor to
its full window, then turns the camera by the cursor's movement from centre.
winemac only switches the Mac pointer into **relative** mode — where movement
arrives as unbounded deltas — when the clip rectangle is a *strict subset* of
the display; a clip covering the whole desktop is treated as "not clipping",
leaving the pointer in **absolute** mode where the position clamps at the
screen edges. Because the borderless window is exactly desktop-sized, the
engine's full-window clip hits that case: push right or down and the position
pins at the edge, the per-frame delta collapses to zero — the wall.

The plugin insets any (essentially) full-display clip by 2 px so it is a
strict subset; winemac then engages relative mode and the wall is gone. A
cursor *release* (menus, where the pointer roams freely) is passed through
untouched. `MouseClipFix=-1` (the default) applies this under Wine only.

### Slow-move camera stall — `MouseMotionFix` (automatic on CrossOver)

With the edge wall gone, one artefact remained: turning the camera **slowly**
stalled, while the same slow motion moved the *menu cursor* fine. The cause
is where the camera's motion comes from. DirectInput's *relative* axis is
lossy on winemac — it gets an integer per-event delta, so a slow move rounds
to zero every event and the camera doesn't move. The win32 cursor position
does **not** lose this: winemac accumulates the fractional motion into the
absolute cursor, so it creeps smoothly at any speed.

The plugin wraps the DirectInput mouse device (via COM vtable, no game
byte-offsets) and, while camera-look is active, **replaces the device's X/Y
with motion derived from the OS cursor** — the smooth source — while passing
the **buttons through untouched**. So you get slow-move camera *and* working
fire from the one path. Menus are left alone. `MouseMotionFix=-1` (the
default) applies this under Wine only.

### Full-res post-filter (`PostFilterFullRes`)

With its post-filter enabled (`PostFilterLOD 1`, the stock default),
Contracts renders the whole 3D scene into offscreen buffers at **quarter
resolution**, runs the bloom/neon-flare/color-grade on them and upscales — so
the graded scene looks soft and blocky. `PostFilterFullRes=1` (the default)
patches the post-buffer allocator so those buffers are allocated at full
backbuffer resolution — full-res 3D *and* the post-effects, while the
fixed-pixel HUD and menus keep their size. The patch verifies the exact bytes
first and no-ops on a build it does not recognize; keep `PostFilterLOD >= 1`
in `HitmanContracts.ini` so the filter path runs (the installer restores it
if a previous run zeroed it).

Two companion options work around CrossOver D3DMetal quirks that would
otherwise make the post-filter composite vanish or the textures go flat:

- `PostFilterOpaqueRT=1` (default): the game's backbuffer keeps its original
  alpha-capable format (needed for detail-texture effects), but selected
  post-filter render targets are created without alpha, so the bloom
  bright-pass — which multiplies scene colour by the surface's alpha — isn't
  zeroed out by the near-zero alpha D3DMetal leaves there.
- `PostFilterOpaqueRTMask=3` chooses which post-filter buffers that covers.
  The default covers exactly the two bloom buffers; the rest keep alpha
  because the dying/slow-motion black-and-white desaturation ramps through a
  buffer's alpha, and forcing that one opaque hangs the death cam. Leave it
  at `3`.
- `PostFilterAlphaFix` (off by default) is an older, blunter workaround for
  the same class of problem, kept for testing.

### UI scaling (`UIScale`)

The engine lays its 2D layer out against the `HitmanContracts.ini`
resolution, so without `UIScale` the UI-size knob is the `Resolution` line —
a smaller resolution = a proportionally bigger UI, coupled with a softer 3D
image (the backbuffer is pinned to it).

`UIScale=N` (N>1) has the same convention everywhere: `Resolution` is always
the render resolution and the UI is laid out at `Resolution/N`. For example,
`Resolution 2560x1600` plus `UIScale=2` renders at 2560x1600 with a
1280x800-sized UI.

The implementation is platform-specific but hidden from configuration. On
Wine/CrossOver the plugin substitutes the divided resolution only in the
engine's `ReadFile` buffer during startup parsing; the physical INI is never
modified. It then uses the fast viewport-only grow path. On native Windows it structurally
identifies and patches the UI settings allocation. This avoids the 15–30 FPS
D3DMetal penalty from per-draw RHW/UV interception while keeping one portable
configuration.

Two Contracts-specific differences from H2SA:

- The scan-and-patch runs at the **first `CreateDevice`**, not at plugin
  load: Contracts imports `d3d8.dll` statically, so the plugins load before
  the engine has parsed its ini (H2SA's plugins load from inside the
  engine's `LoadLibrary(RenderD3D.dll)`, after the parse). The device does
  not exist yet at that point, so no device-derived copies can false-match.
- Negative factors are retained for backward compatibility with the earlier
  explicit grow-mode configuration; positive factors are recommended.

While re-believed, the in-game resolution switch is locked for the session
(the engine's patched belief cannot be re-pointed mid-run; a switch is
logged and applies on the next launch). If the engine saves the divided
layout value back into `HitmanContracts.ini` on exit, a detach-time guard
restores the render resolution so repeated launches cannot divide it twice.

In legacy positive-factor mode, the **post-filter (bloom/colour-grade) runs
under UIScale**: the loader
treats every render target of the full backbuffer size as a believed-space
canvas (the engine renders its scene, menus and post passes through such
RTs and blits them 1:1), rescaling the believed-space viewports and RHW
quads inside them, and rescaling the **texture coordinates** of draws that
sample those full-size RTs (the engine computes them as layout-derived
sub-rects; the content now fills the RT, so the UVs follow — full-range
0..1 blits are unaffected by the scale-and-clamp). Device-space geometry
(e.g. the video player's quads) is detected by its coordinates and left
alone.

Only in legacy positive-factor mode, the engine's believed-space **2D layer**
may be rescaled. That layer
emits pre-transformed quads with `rhw` exactly 1 and `z` in 0..1, and its
vertex stride matches the tracked FVF — any RHW draw failing those checks
(e.g. software-projected world geometry, whose on-screen coordinates can
also happen to fit inside the layout rect) is left untouched. Without this
gate such scene draws were displaced/corrupted — including the canvas's
destination-alpha channel, which brought the "invisible/flat ground
textures" of the old X8-backbuffer bug back in the same spots. Skipped
draws log one-shot `UIScale: ... left unscaled` lines; `UIScaleStrict2D=0`
restores the old bounds-only classification if a UI element ever stops
scaling.

Legacy-mode vertex-buffer UI quads are transformed in place at the end of the engine's
existing write lock. This is deliberately not done by reopening buffers from
the draw hook: a read lock serializes D3DMetal, and redirecting the result
through `Draw*UP` adds a second upload. That older path cost roughly 15 FPS on
CrossOver even though each individual UI quad contains only four vertices.

If the post path ever misbehaves, `UIScalePostFilter=0` is the
fallback: it forces the engine's parsed `PostFilterLOD` to 0 in memory
(re-asserted every frame — an ini edit would not stick, the engine re-saves
that value from its detail setting on every exit), rendering the scene
directly into the backbuffer with no post effects.

### Rain performance caps

The heaviest rain scenes are CPU-bound in the game's rain particle vertex
builder. With the x87 plugin installed that builder is translated to SSE2 and
the cost largely disappears at the source, so the caps are a fallback:
`RainEmitCap=N` clamps the number of rain quads one visible rain pass emits,
and `RainSystemCap=N` limits how many visible rain systems are processed per
frame. Both default to `0` (off); lower values are faster but can visibly
thin dense rain — compare the same camera angle against `0`.

Config: the `[display]` section of `scripts/hmc_display.ini` (the `[profiler]`
section is described below)

```ini
[display]
Enabled=1
Fullscreen=0        ; 1 = exclusive fullscreen (real Windows only, at an
                    ; enumerated mode); on Wine/Mac -> borderless-fullscreen
Borderless=-1       ; when not fullscreen: -1 auto = borderless fills desktop,
                    ; 0 plain window, 1 always borderless
FOVCorrect=1        ; Hor+ projection correction on/off
FOVFactor=1.0       ; extra horizontal FOV multiplier (>1 = wider)
CursorFix=0         ; hide stray macOS cursor + startup activation: 0 off
                    ; (default), 1 on, -1 auto (on under Wine)
FpsCap=60           ; frame-rate cap; 0 = uncapped
VSync=-1            ; -1 auto / 0 off => immediate present (the software cap
                    ; is the sole pacer); 1 = force vsync-every-frame
BackBuffers=2       ; backbuffer count (2 => triple-buffered)
PostFilterFullRes=1 ; full-resolution post-filter effects (needs
                    ; PostFilterLOD >= 1 in HitmanContracts.ini)
PostFilterAlphaFix=0 ; older CrossOver workaround; leave off
PostFilterOpaqueRT=1 ; CrossOver workaround so bloom survives; leave on
PostFilterOpaqueRTMask=3 ; which buffers it covers; leave at 3 (see above)
RainEmitCap=0       ; rain CPU limiter, quads per pass; 0 = off
RainSystemCap=0     ; rain CPU limiter, systems per frame; 0 = off
ForceWinMouse=-1    ; mouse buttons fix: -1 auto (on under Wine), 0 off, 1 on
MouseClipFix=-1     ; mouse-look edge-wall fix: -1 auto, 0 off, 1 on
MouseMotionFix=-1   ; slow-move stall fix: -1 auto, 0 off, 1 on
UIScale=0           ; N>1 = render at ini Resolution, UI laid out at
                    ; Resolution/N; 0/1 = off
UIScalePostFilter=1 ; keep the post-filter under UIScale (default); 0 =
                    ; force PostFilterLOD 0 in memory instead (fallback)
UIScaleStrict2D=1   ; only rescale true 2D-layer draws (rhw==1, z in 0..1,
                    ; stride matching the FVF); 0 = old bounds-only test
```

Install output: loader `d3d8.dll` (game root); `scripts/hmc_display.asi` +
`.ini`; logs `scripts/hmc_asi_loader.log`, `scripts/hmc_display.log`.

### Profiler overlay

A small performance overlay in the top-right corner, drawn each frame through
the loader's `on_frame` hook (a built-in pixel font rendered via
`DrawPrimitiveUP` with full state save/restore — no game offsets). It shows:

```text
FPS 60
MS 16.6/22.0        ; average / peak frame time this window
X87 38%             ; CPU in the SSE2-translated blob
GAME 45%            ; CPU in HitmanContracts.exe (untranslated: renderer +
                    ; game logic + any leftover x87)
REST 17%            ; everything else (D3D/Metal, wine, system)
```

The CPU split comes from a background thread that samples the game's main
thread instruction pointer and buckets it by address range. It follows the
`hmc_reduced_x87` entry hooks to find the translated blob, so blob samples are
attributed to **X87** — making the x87 translation's effect directly visible
(time that was emulated x87 now shows up as native SSE2 under X87, moving out
of GAME). Because Contracts is monolithic there is no separate renderer
module, so untranslated renderer code and game logic both fall under GAME.

Config: the `[profiler]` section of `scripts/hmc_display.ini`

```ini
[profiler]
Enabled=1      ; 0 hides the overlay (sampler still idle-cheap)
Scale=1.0      ; text size; 2.0 doubles it
ShowCPU=0      ; 0 = FPS + frame time only (default); 1 = add the
               ; X87/GAME/REST breakdown. Sampling suspends the game's main
               ; thread 250x/second and costs ~8-15% frame time — leave it
               ; off for play, turn on only to diagnose.
OffsetX=8      ; inset from the right edge, in pixels
OffsetY=8      ; inset from the top edge, in pixels
```

The profiler's log lines (tagged `[profiler]` in `scripts/hmc_display.log`)
are a periodic text snapshot of the same stats plus a `REST-top:` breakdown
naming the modules the "REST" time lands in (e.g. `wined3d.dll`, `d3d8.dll`,
`ntdll.dll`).

## Plugin: Reduced x87 (`hmc_reduced_x87.asi`)

Improves performance under CrossOver/Rosetta 2, where x87 instructions are
emulated in software (80-bit) while SSE2 runs on hardware. An offline
translator rewrites provably-safe x87 functions into SSE2 double-precision
code; the runtime loader installs 5-byte entry hooks when
`HitmanContracts.exe` is up. The translator toolchain is shared with the
Hitman 2 / C47 builds (`tools/x87/`).

Because Contracts is a single unpacked exe, the whole of its x87 — the
renderer's vertex/lighting math **and** the AI / animation / physics
game-logic math — is one translation target, read straight from the
executable. Coverage measured on the retail build (Steam, Build 116):

```text
translated 1513 functions, 60237 x87 insns  (~96% of analyzed)
```

Precision: translated code computes in 64-bit doubles instead of 80-bit
extended — more than enough for a renderer whose results land in 32-bit float
vertex buffers, and for game-logic math. Anything the translator cannot prove
safe (unknown x87 stack depth, jump tables, unbalanced call/return state)
stays original.

Generate the patch blob and build the plugin:

```sh
pip3 install capstone pefile
python3 tools/translate.py            # writes dist/HitmanContracts.exe.x87
(cd runtime && make)                  # builds dist/hmc_reduced_x87.asi
./install.sh                          # installs the .asi + the .x87 blob
```

The plugin is byte-checked: if `HitmanContracts.exe`'s PE timestamp or image
size does not match the blob, it logs a mismatch and applies nothing. It also
verifies each function's live entry bytes against the recorded originals
before hooking, so an unexpected entry is skipped rather than mis-hooked.

Install output: `scripts/hmc_reduced_x87.asi`; patch blob
`scripts/hmc_reduced_x87/HitmanContracts.exe.x87`; log
`scripts/hmc_reduced_x87.log`. Expected log line after launch:

```text
[hitmancontracts.exe] applied: 1514/1514 hooks (0 skipped), blob 1040 KB at ... (delta ...)
```

A diagnostic build (`hmc_reduced_x87_diag.asi`, built on demand with
`make diag`) adds a
NaN/Inf tripwire at float-returning translated functions — useful if a
translated function is suspected of misbehaving. If one does, exclude it by
RVA: `python3 tools/translate.py --exclude 0x1234` (or list RVAs in
`tools/exclusions/hitmancontracts.exe.txt`).

Some functions are only ever reached through C++ vtables and are missed by
automatic discovery (the exe ships without relocation data, which discovery
would otherwise use to find them). Such entries can be seeded by RVA in
`tools/starts/hitmancontracts.exe.txt` — the rain particle builder, once the
game's biggest remaining x87 hotspot, is included this way. Only add an RVA
that is a real function entry.

## ASI Loader (`d3d8.dll`)

A minimal d3d8.dll proxy (`runtime/asiloader.c`). On attach it loads every
`*.asi` from `scripts/` and logs to `scripts/hmc_asi_loader.log`. It exports
all five real d3d8 entry points at their real ordinals (see
`runtime/d3d8.def`); four are forwarded to the system d3d8, resolved lazily,
and `Direct3DCreate8` is wrapped so the returned `IDirect3D8` — and the
`IDirect3DDevice8` it creates — have their `CreateDevice`, `Reset`,
`Present`, `SetTransform` and `SetViewport` vtable slots redirected here.
Plugins opt in via `HMC_RegisterD3D8Hooks` (see `runtime/hmc_d3d8.h`); the
vtable layout is a fixed COM ABI, so no game offsets are involved.

Under Wine/CrossOver the bottle needs `d3d8=native,builtin` so the
game-directory proxy is chosen over the builtin d3d8 (which is what the proxy
forwards to). `install.sh` sets this automatically for CrossOver bottles.
Extra plugins dropped into `scripts/` are loaded too; a new plugin that needs
the D3D8 hooks just registers through the same API.

## Verification

### Widescreen / startup (`tests/harness.c`)

`tests/harness.c` reproduces what the game does — create a window,
`Direct3DCreate8`, then `CreateDevice` with exclusive-fullscreen parameters —
against the game-directory proxy, so the whole proxy → plugin → device path
can be exercised without launching the game. For the real game, launch
through Steam and inspect `scripts/hmc_asi_loader.log` and
`scripts/hmc_display.log`: they record the requested vs. applied
presentation parameters and the `CreateDevice` result.

### Draw-cost probe

For performance captures (e.g. rainy scenes), enable the loader's opt-in draw
probe with `HMC_DRAWSTATS=1` or an empty `scripts/DRAWSTATS` marker file
before launch. `scripts/hmc_asi_loader.log` then emits one line per 60-frame
window with FPS, draw counts, vertex-buffer lock time, Present time and rain
stats — enough to tell CPU submission cost apart from GPU/present cost when
chasing a dip.

### x87 translation (`tests/difftest.c`)

`tests/difftest.c` validates the SSE2 translation against the original x87
code. It loads `HitmanContracts.exe` as a code image (no entry point, no
imports), maps the translated blob, then calls every leaf function both ways
with identical randomized register/stack/memory contexts and compares the
results (float-tolerant, since translated code is 64-bit double vs 80-bit
extended).

```sh
python3 tools/translate.py            # dist/HitmanContracts.exe.x87
python3 tools/gen_manifest.py         # dist/HitmanContracts.exe.leaf.txt
tests/difftest.sh                     # build + run in the CrossOver bottle
tests/difftest.sh 60 rich             # first 60 funcs, pointer-rich contexts
```

A clean run reports `0 mismatched`. Functions that dereference pointers
supplied as random arguments fault identically in both versions and are
reported as `all-fault` (the tester cannot fabricate a valid context for
them) — that is not a translation defect. Verified baseline on this build:

```text
60 tested: 44 passed, 0 mismatched, 16 all-fault
```

The strongest evidence, as always, is the game running stably with all 1,513
translated functions executing every frame.
