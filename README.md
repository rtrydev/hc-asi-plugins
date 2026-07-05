# HMC ASI Plugins (Hitman: Contracts)

ASI plugins and a bundled ASI loader for **Hitman: Contracts** on modern
systems, especially CrossOver/Rosetta 2 on Apple Silicon Macs. It is the
sibling of the [Hitman 2: Silent Assassin build](../h2sa-asi-plugins) and the
[Hitman: Codename 47 build](../hc47-asi-plugins). Contracts runs on the same
Glacier/RenderD3D lineage as Hitman 2 and is likewise a **Direct3D 8** title,
so the loader-and-hooks approach is shared — but Contracts differs in one
structural way that simplifies the whole suite (see *Architecture* below).

The repo builds four artifacts:

- `d3d8.dll` — a Direct3D 8 proxy that doubles as the ASI loader. It loads
  every `*.asi` from `scripts/`, forwards the real d3d8 exports to the system
  d3d8, and wraps the D3D8 COM interface so plugins can influence device
  creation and rendering without patching game code.
- `HMCWidescreen.asi` — makes the game **start** under CrossOver (the stock
  exclusive-fullscreen device fails there) and renders it in correct widescreen
  at any resolution.
- `HMCReducedX87.asi` — translates the game's x87 float code to SSE2 at runtime
  for a large CrossOver/Rosetta 2 performance win (see below).
- `HMCProfiler.asi` — a top-right on-screen overlay showing FPS, frame time,
  and an EIP-sampled CPU-time breakdown (see below).

The widescreen/startup fix hooks the fixed Direct3D 8 COM ABI, so it has **no
game-build byte offsets** to break. The x87 plugin patches function entries,
but every site is byte-checked against the retail build (PE timestamp + image
size + per-function entry bytes) and it declines to patch a module it does not
recognise.

## Architecture — how Contracts differs from Hitman 2

Hitman 2 is split across DLLs: `hitman2.exe` (packed) loads `RenderD3D.dll` at
run time, and only that DLL imports `Direct3DCreate8`. **Contracts is a single
monolithic, *unpacked* executable**: the RenderD3D renderer, the sound engine
and the SDL script engine are all statically linked into
`HitmanContracts.exe`, and the exe imports `Direct3DCreate8` from `d3d8.dll`
in its own import table (confirmed: `.text` entropy ≈ 6.5, i.e. ordinary code,
and `IMPORTS: … d3d8.dll …`). Three consequences run through the whole suite:

1. **The d3d8.dll proxy still attaches perfectly** — it is mapped at process
   start (a static import), before the game creates its device.
2. **The x87 translation targets `HitmanContracts.exe` directly**, read
   straight from the on-disk image. There is **no memory-dump step** (Hitman 2
   needed one because its exe ships packed). That is why this repo has no
   `dumper.c` / `undump.py`.
3. **The profiler collapses to one game module.** There is no separate renderer
   DLL to bucket apart from game logic, so the CPU split is X87 (translated
   blob) / GAME (everything untranslated in the exe) / REST.

Contracts also shares Hitman 2's exact startup failure string —
*"Unable to create device. Try changing resolution or color depth"* — so the
D3D8 windowed/borderless startup fix applies unchanged.

## Build & Install

### mac

```sh
brew install mingw-w64
pip3 install capstone pefile   # for the x87 plugin's translation blob
python3 tools/translate.py     # writes dist/HitmanContracts.exe.x87
(cd runtime && make)           # writes dist/d3d8.dll + the .asi plugins
./install.sh                   # copies them into the game + configures the bottle
./install.sh -u                # uninstall (leaves your plugin .ini files)
```

The `translate.py` step is only for `HMCReducedX87.asi`; the loader, widescreen
and profiler plugins build without it. `install.sh` installs the x87 plugin
only if both `dist/HMCReducedX87.asi` and `dist/HitmanContracts.exe.x87` exist.

By default `install.sh` targets:

```text
mac:     /Users/rtry/Library/Application Support/CrossOver/Bottles/Steam/drive_c/Program Files (x86)/Steam/steamapps/common/Hitman Contracts
Windows: C:\Program Files (x86)\Steam\steamapps\common\Hitman Contracts
```

Override with `HMC_GAME_DIR=/path/to/game ./install.sh`.

On mac the installer also configures the bottle:

- `d3d8=native,builtin` DLL override, so the game-directory proxy wins over the
  builtin d3d8.
- `Mac Driver\CaptureDisplaysForFullscreen=y`, so winemac captures the display
  for a borderless-fullscreen window — which is what hides the macOS menu bar
  and stops the host (Mac) cursor showing through near the Dock.
- ensures `HitmanContracts.ini` has a `Resolution WxH` line (it has none by
  default). If absent, `1920x1080` is added; a small 4:3 placeholder is bumped.
  Backup: `HitmanContracts.ini.bak`; override with `HMC_RESOLUTION=WxH`.

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

Launch the game the normal way (through the Steam client, appid `247430`), not
by running `HitmanContracts.exe` directly, so the game's Steam check passes.

## Plugin: Widescreen + startup fix (`HMCWidescreen.asi`)

Two problems on a modern Mac, both handled at the D3D8 boundary:

### Startup — "Unable to create device"

In its default configuration the renderer asks D3D8 for an **exclusive
fullscreen** device at the configured resolution and colour depth. Under the
CrossOver D3DMetal/wined3d stack that `CreateDevice` fails, and the game then
shows its own fatal box — *"Unable to create device. Try changing resolution or
color depth"*. The fix is the same as the Hitman 2 build: **don't use exclusive
fullscreen.** The plugin rewrites the presentation parameters to windowed
(which uses the desktop format and always creates) and, by default under Wine,
strips the window to a borderless popup filling the desktop so it still looks
fullscreen.

### Widescreen — resolution and aspect

The resolution comes from the `Resolution WxH` line in `HitmanContracts.ini`
(the installer adds one — the stock ini has none). The plugin **pins the
backbuffer to the ini resolution** — the same value the engine lays its
viewport, HUD and mouse mapping out against — and applies the standard **Hor+**
projection correction (scale the projection x term by `(4/3)/aspect`) so the
horizontal FOV widens to the true aspect while the vertical FOV is preserved.
Orthographic (2D/HUD) projections are detected and left untouched.

Two runtime clamps make arbitrary resolutions robust even where the engine's
own mode-selection misbehaves, entirely at the D3D8 ABI (no byte offsets):

- the loader's **SetViewport hook** clamps any out-of-range viewport back to
  the backbuffer (a garbage viewport otherwise maps the scene to a sliver);
- **fix_projection** rebuilds a collapsed vertical scale from the real aspect.

#### On the resolution-snap byte patch

Hitman 2's RenderD3D snaps the requested resolution to a fixed 4:3 ladder and
loads a garbage height past the top of it; the H2SA build neutralises that with
a 16-byte signature patch on the guarding `je`. Contracts uses the same
renderer lineage, but it is compiled *into the exe*, so the exact byte
signature is this build's, not Hitman 2's. The plugin keeps the Hitman 2
signature as an **opportunistic** patch: if it happens to match this build it
disables the ladder at the source; otherwise (the expected case) it logs that
the signature is absent and **relies on the runtime clamps above**, which need
no signature and are inherently safe (they only ever narrow an out-of-bounds
viewport and correct an already-broken projection). So the suite delivers
widescreen with or without the byte patch. If a byte-exact ladder signature for
this build is confirmed on-target, drop it into `SNAP_SIG` in `widescreen.c`.

### Fullscreen vs. borderless

By default the game runs **borderless** — a frameless window filling the screen,
aspect-preserving. This always creates, survives alt-tab, and needs no display
mode switch. **Exclusive fullscreen** (`Fullscreen=1`) is **real-Windows only**
(and only at an enumerated display mode); on Wine/CrossOver it is broken
(winemac drives the captured display at a 2× Retina backing scale, so a
fullscreen surface renders larger than the panel) and is treated as
borderless-fullscreen, which fills the screen correctly.

### Frame-rate cap

The Glacier engine advances its simulation from the measured frame time, so an
uncapped modern GPU makes physics, camera and scripted timing run wild. The
loader wraps `IDirect3DDevice8::Present` and the plugin paces each frame to hold
`FpsCap` (default **60**). Set `FpsCap=0` to disable. Applies on every platform.

Config: `scripts/HMCWidescreen.ini`

```ini
[Widescreen]
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
VSync=-1            ; -1 auto / 0 off => immediate present (software cap is the
                    ; sole pacer — avoids the limiter-vs-vsync beat on ProMotion
                    ; panels); 1 = force vsync-every-frame
BackBuffers=2       ; backbuffer count (2 => triple-buffered); overlaps CPU/GPU
                    ; so Present doesn't serialize the frame
PostFilterFullRes=1 ; render the post-filter (bloom/neon flares + color grade)
                    ; at full backbuffer resolution instead of the game's
                    ; hardwired quarter-res buffer (needs PostFilterLOD >= 1 in
                    ; HitmanContracts.ini). See "Full-res post-filter" below.
ForceWinMouse=-1    ; fix dead mouse buttons under winemac: -1 auto (on under
                    ; Wine, off on real Windows), 0 off, 1 always. See below.
```

### Mouse buttons — `ForceWinMouse` (automatic on CrossOver)

Contracts defaults to reading the mouse through **DirectInput**, and winemac
delivers DirectInput mouse *motion* but not *button* state — so the cursor
moves and menu items highlight on hover, but **clicks and firing do nothing**,
on every device. The game switches to a working (Windows) mouse path purely
based on whether `UseDirectInputMouse` is *present* in `HitmanContracts.ini`
(the value is ignored). Rather than require that edit, the widescreen plugin
patches the one instruction that reads it so the working path is always taken —
so **the mouse just works, with no game-ini change**. `ForceWinMouse=-1` (the
default) applies this under Wine only; on real Windows the DirectInput path
already reports buttons, so it is left alone. The patch runs before the game
reads its config and byte-checks the site, no-opping on an unrecognised build.

### Frame pacing — the limiter fix

The frame limiter had a bug (inherited from the Hitman 2 build) where a frame
that ran *longer* than the cap period got a full extra period of sleep piled on
— roughly **halving** the frame rate of any scene that dips below the cap.
Hitman 2 / Codename 47 never dip below 60 so it never surfaced there; Contracts'
heavy scenes do, so it showed up as a hard drop to ~28fps with the main thread
parked in `ntdll` (the limiter's own sleep). Fixed: when behind schedule the
limiter now presents immediately instead of sleeping.

### Full-res post-filter (`PostFilterFullRes`)

With its post-filter enabled (`PostFilterLOD 1`, the stock default), Contracts
renders the whole 3D scene into offscreen buffers sized **backbuffer ÷ 2 per
axis** (quarter-resolution), runs the bloom/neon-flare/color-grade on them and
upscales — so the graded scene looks soft/blocky. The ini value is `atoi`'d, so
it is on/off only (`0` disables the post-pass entirely: sharp but no effects),
and the buffer size ignores the value. `PostFilterFullRes=1` NOPs the six
`shr ecx,1` (÷2) instructions in the post-buffer allocator so those buffers are
allocated at full backbuffer resolution — **full-res 3D *and* the post-effects,
at a normal 1920×1080 backbuffer** (so the fixed-pixel HUD/menus keep their
size). The ÷4 bloom buffer is left small (a downsampled bloom is correct and
cheap). The patch verifies the exact bytes first and no-ops on a build it does
not recognise; keep `PostFilterLOD >= 1` in `HitmanContracts.ini` so the filter
path runs.

Install output: loader `d3d8.dll` (game root); `scripts/HMCWidescreen.asi` +
`.ini`; logs `scripts/HMCAsiLoader.log`, `scripts/HMCWidescreen.log`.

## Plugin: Reduced x87 (`HMCReducedX87.asi`)

Improves performance under CrossOver/Rosetta 2, where x87 instructions are
emulated in software (80-bit) while SSE2 runs on hardware. An offline
translator rewrites provably-safe x87 functions into SSE2 double-precision
code; the runtime loader installs 5-byte entry hooks when `HitmanContracts.exe`
is up. The translator toolchain is shared with the Hitman 2 / C47 builds
(`tools/x87/`).

Because Contracts is a single unpacked exe, the whole of its x87 — both the
renderer's software vertex-transform/lighting math **and** the AI / animation /
physics game-logic math — is one translation target, read straight from the
executable. Coverage measured on the retail build (Steam, Build 116):

```text
x87 insns in module (census): 105387
translatable: 1513 functions
translated 1513 functions, 60237 x87 insns  (~96% of analyzed, ~57% of census)
blob 1033 KB, 15589 fixups
```

Precision: translated code computes in 64-bit doubles instead of 80-bit
extended — more than enough for a renderer whose results land in 32-bit float
vertex buffers, and for game-logic math. Anything the translator cannot prove
safe (unknown x87 stack depth, `fprem`, jump tables, EFLAGS hazards, unbalanced
call/return state) stays original.

Generate the patch blob and build the plugin:

```sh
pip3 install capstone pefile
python3 tools/translate.py            # writes dist/HitmanContracts.exe.x87
(cd runtime && make)                  # builds dist/HMCReducedX87.asi
./install.sh                          # installs the .asi + the .x87 blob
```

The plugin is byte-checked: if `HitmanContracts.exe`'s PE timestamp or image
size does not match the blob, it logs a mismatch and applies nothing. It also
verifies each function's live entry bytes against the recorded originals before
hooking (so a partially-modified or relocated entry is skipped rather than
mis-hooked).

Install output: `scripts/HMCReducedX87.asi`; patch blob
`scripts/HMCReducedX87/HitmanContracts.exe.x87`; log
`scripts/HMCReducedX87.log`. Expected log line after launch:

```text
[hitmancontracts.exe] applied: 1513/1513 hooks (0 skipped), blob 1033 KB at ... (delta ...)
```

A diagnostic build (`HMCReducedX87-diag.asi`, also built by `make`) adds a
NaN/Inf tripwire at float-returning translated functions and logs the FPU
control word and helper-call counts. If a specific function misbehaves, exclude
it by RVA: `python3 tools/translate.py --exclude 0x1234` (or list RVAs in
`tools/exclusions/hitmancontracts.exe.txt`).

## Plugin: Profiler (`HMCProfiler.asi`)

A small performance overlay in the top-right corner, drawn each frame through
the loader's `on_frame` hook (a built-in 5×7 pixel font rendered as
pre-transformed colored triangles via `DrawPrimitiveUP`, wrapped in a state
block save/restore — so it needs no game offsets). It shows:

```text
FPS 60
MS 16.6/22.0        ; average / peak frame time this window
X87 38%             ; CPU in the SSE2-translated blob
GAME 45%            ; CPU in HitmanContracts.exe (untranslated: renderer +
                    ; game logic + any leftover x87)
REST 17%            ; everything else (D3D/Metal, wine, system)
```

The CPU split comes from a background EIP-sampling thread that reads the game's
main thread instruction pointer and buckets it by address range. It follows the
`HMCReducedX87` entry hooks to find the translated blob, so blob samples are
attributed to **X87** — making the x87 translation's effect directly visible
(time that was emulated x87 inside the exe now shows up as native SSE2 under
X87, moving out of GAME).

Because Contracts is monolithic there is no separate renderer module to split
out, so untranslated renderer code and untranslated game logic both fall under
**GAME**. With `ShowCPU=0` the sampler still runs but only FPS/frame time is
drawn.

Config: `scripts/HMCProfiler.ini`

```ini
[Profiler]
Enabled=1      ; 0 hides the overlay (sampler still idle-cheap)
Scale=1.0      ; text size; 2.0 doubles it
ShowCPU=0      ; 0 = FPS + frame time only (default); 1 = add the X87/GAME/REST
               ; CPU breakdown. The breakdown SUSPENDS the game's main thread
               ; 250x/second to sample its EIP, which itself costs ~8-15% frame
               ; time — so leave it off for play, turn on only to diagnose.
OffsetX=8      ; inset from the right edge, in pixels
OffsetY=8      ; inset from the top edge, in pixels
```

Install output: `scripts/HMCProfiler.asi` + `.ini`; log
`scripts/HMCProfiler.log` — a periodic text snapshot of the same stats plus a
`REST-top:` breakdown naming the modules the "REST" time lands in (e.g.
`wined3d.dll`, `d3d8.dll`, `ntdll.dll`).

## ASI Loader (`d3d8.dll`)

A minimal d3d8.dll proxy (`runtime/asiloader.c`). On attach it loads every
`*.asi` from `scripts/` and logs to `scripts/HMCAsiLoader.log`. It exports all
five real d3d8 entry points at their real ordinals (see `runtime/d3d8.def`);
four are forwarded to the system d3d8, resolved lazily, and `Direct3DCreate8`
is wrapped so the returned `IDirect3D8` — and the `IDirect3DDevice8` it creates
— have their `CreateDevice`, `Reset`, `Present`, `SetTransform` and
`SetViewport` vtable slots redirected here. Plugins opt in via
`HMC_RegisterD3D8Hooks` (see `runtime/hmc_d3d8.h`); the vtable layout is a fixed
COM ABI, so no game offsets are involved.

Under Wine/CrossOver the bottle needs `d3d8=native,builtin` so the
game-directory proxy is chosen over the builtin d3d8 (which is what the proxy
forwards to). `install.sh` sets this automatically for CrossOver bottles. Extra
plugins dropped into `scripts/` are loaded too; a new plugin that needs the D3D8
hooks just registers through the same API.

## Verification

### Widescreen / startup (`tests/harness.c`)

`tests/harness.c` reproduces what the renderer does — create a window,
`Direct3DCreate8`, then `CreateDevice` with exclusive-fullscreen parameters —
against the game-directory proxy, so the whole proxy → plugin → device path can
be exercised without launching the game. For the real game, launch through
Steam and inspect `scripts/HMCAsiLoader.log` and `scripts/HMCWidescreen.log`:
they record the requested vs. applied presentation parameters and the
`CreateDevice` result.

### x87 translation (`tests/difftest.c`)

`tests/difftest.c` validates the SSE2 translation against the original x87 code.
It loads `HitmanContracts.exe` as a code image (with
`DONT_RESOLVE_DLL_REFERENCES` — no entry point, no imports), maps the translated
blob with fixups applied, then calls every leaf function (from
`dist/HitmanContracts.exe.leaf.txt`, produced by `tools/gen_manifest.py`) both
ways with identical randomized register/stack/memory contexts and compares
`eax`/`edx`/`st0` and all touched scratch memory (float-tolerant, since
translated code is 64-bit double vs 80-bit extended).

```sh
python3 tools/translate.py            # dist/HitmanContracts.exe.x87
python3 tools/gen_manifest.py         # dist/HitmanContracts.exe.leaf.txt
tests/difftest.sh                     # build + run in the CrossOver bottle
tests/difftest.sh 60 rich             # first 60 funcs, pointer-rich contexts
```

A clean run reports `0 mismatched`. Functions that dereference pointers supplied
as random arguments fault identically in both versions and are reported as
`all-fault` (the tester cannot fabricate a valid context for them) — that is not
a translation defect. Verified baseline on this build:

```text
60 tested: 44 passed, 0 mismatched, 16 all-fault
```

The strongest evidence, as always, is the game running stably with all 1,513
translated functions executing every frame.

## Status / what is verified where

Everything that can be checked offline has been:

- **Direct3D 8 + monolithic-unpacked exe** — confirmed by the exe's import
  table (`d3d8.dll` / `Direct3DCreate8`) and `.text` entropy.
- **All artifacts build** with mingw-w64 (`d3d8.dll` + three `.asi` +
  diagnostic `.asi`), and the loader exports the five d3d8 entry points at the
  correct ordinals plus `HMC_RegisterD3D8Hooks`.
- **x87 translation** — 1,513 functions / 60,237 x87 insns translated; the
  differential tester reports **0 mismatched** on a CrossOver run.

The pieces that need on-target confirmation (a running game under CrossOver) are
the usual runtime-behaviour items: the exact borderless fill / cursor feel, and
whether this build's resolution-mode path needs the optional `SNAP_SIG` byte
patch or is fully served by the runtime viewport/projection clamps.
