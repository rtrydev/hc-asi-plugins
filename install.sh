#!/bin/bash
# Install (or uninstall with -u) the HMC (Hitman: Contracts) plugins into the game.
# Works on mac (CrossOver bottle) and on Windows under Git Bash / MSYS2.
set -e
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*)
        DEFAULT_GAME="/c/Program Files (x86)/Steam/steamapps/common/Hitman Contracts";;
    *)
        DEFAULT_GAME="$HOME/Library/Application Support/CrossOver/Bottles/Steam/drive_c/Program Files (x86)/Steam/steamapps/common/Hitman Contracts";;
esac
GAME="${HMC_GAME_DIR:-$DEFAULT_GAME}"
HERE="$(cd "$(dirname "$0")" && pwd)"

if [ "$1" = "-u" ]; then
    rm -f "$GAME/d3d8.dll"
    rm -f "$GAME/scripts/HMCAsiLoader.log"
    rm -f "$GAME/scripts/HMCWidescreen.asi"
    rm -f "$GAME/scripts/HMCWidescreen.log"
    rm -f "$GAME/scripts/HMCReducedX87.asi"
    rm -f "$GAME/scripts/HMCReducedX87.log"
    rm -rf "$GAME/scripts/HMCReducedX87"
    rm -f "$GAME/scripts/HMCProfiler.asi"
    rm -f "$GAME/scripts/HMCProfiler.log"
    # the plugin .ini is user config; left in place on purpose.
    # HitmanContracts.ini is not touched on uninstall; restore
    # HitmanContracts.ini.bak by hand to undo the Resolution line.
    echo "uninstalled (d3d8.dll + plugins removed; HitmanContracts.ini left as-is)"
    exit 0
fi

[ -f "$HERE/dist/d3d8.dll" ] || { echo "build first: (cd runtime && make)"; exit 1; }
[ -f "$HERE/dist/HMCWidescreen.asi" ] || { echo "build first: (cd runtime && make)"; exit 1; }
[ -d "$GAME" ] || { echo "game dir not found: $GAME (set HMC_GAME_DIR)"; exit 1; }

mkdir -p "$GAME/scripts"

# ASI loader: d3d8.dll proxy in the game root (also carries the D3D8 hooks)
cp "$HERE/dist/d3d8.dll" "$GAME/d3d8.dll"

# Widescreen + startup fix plugin
cp "$HERE/dist/HMCWidescreen.asi" "$GAME/scripts/"
if [ ! -f "$GAME/scripts/HMCWidescreen.ini" ]; then
    printf '[Widescreen]\nEnabled=1\nFullscreen=0\nBorderless=-1\nFOVCorrect=1\nFOVFactor=1.0\nPreserveAspect=1\nModernModes=1\nCursorFix=0\nFpsCap=60\nVSync=-1\nBackBuffers=2\nPostFilterFullRes=1\nPostFilterAlphaFix=1\nPostFilterOpaqueRT=1\nPostFilterOpaqueRTMask=3\nRainEmitCap=192\nRainSystemCap=32\nForceWinMouse=-1\nMouseClipFix=-1\nMouseMotionFix=-1\n' \
        > "$GAME/scripts/HMCWidescreen.ini"
fi

# Reduced-precision x87 plugin: the SSE2-translated HitmanContracts.exe blob for
# CrossOver/Rosetta 2 performance. The .asi loads any <module>.x87 blob from
# scripts/HMCReducedX87/. Contracts is a single unpacked exe, so there is just
# one blob (HitmanContracts.exe.x87) — no separate renderer DLL, no dump step.
if [ -f "$HERE/dist/HMCReducedX87.asi" ] && [ -f "$HERE/dist/HitmanContracts.exe.x87" ]; then
    cp "$HERE/dist/HMCReducedX87.asi" "$GAME/scripts/"
    mkdir -p "$GAME/scripts/HMCReducedX87"
    cp "$HERE/dist/HitmanContracts.exe.x87" "$GAME/scripts/HMCReducedX87/"
    echo "x87 plugin: HMCReducedX87.asi + HitmanContracts.exe.x87 installed"
else
    echo "NOTE: x87 plugin not installed (run tools/translate.py, then rebuild)"
fi

# Performance profiler overlay (top-right): FPS/frame time + EIP-sampled CPU
# share (translated x87 blob vs untranslated exe vs rest).
if [ -f "$HERE/dist/HMCProfiler.asi" ]; then
    cp "$HERE/dist/HMCProfiler.asi" "$GAME/scripts/"
    if [ ! -f "$GAME/scripts/HMCProfiler.ini" ]; then
        # ShowCPU=0 by default: the CPU-share breakdown suspends the game's main
        # thread to sample it, which costs frame time — leave it off for play and
        # set ShowCPU=1 only when you want the X87/GAME/REST diagnostic.
        printf '[Profiler]\nEnabled=1\nScale=1.0\nShowCPU=0\nOffsetX=8\nOffsetY=8\n' \
            > "$GAME/scripts/HMCProfiler.ini"
    fi
    echo "profiler: HMCProfiler.asi installed (top-right overlay)"
fi

# Give widescreen out of the box. HitmanContracts.ini has no "Resolution" line
# by default (the engine falls back to a built-in default), and the widescreen
# plugin pins the backbuffer to this line, so make sure one exists. Set
# HMC_RESOLUTION=WxH to override, or to your display resolution for a
# pixel-exact borderless fill. The original is backed up to
# HitmanContracts.ini.bak.
RES="${HMC_RESOLUTION:-1920x1080}"
INI="$GAME/HitmanContracts.ini"
if [ -f "$INI" ]; then
    if grep -qiE '^[[:space:]]*Resolution[[:space:]]+[0-9]+x[0-9]+' "$INI"; then
        # a Resolution line exists: only rewrite a small 4:3 placeholder
        if grep -qiE '^[[:space:]]*Resolution[[:space:]]+(640x480|800x600|1024x768)' "$INI"; then
            cp "$INI" "$INI.bak"
            sed "s/^[[:space:]]*[Rr]esolution[[:space:]].*/Resolution ${RES}/" "$INI" > "$INI.tmp" \
                && mv "$INI.tmp" "$INI"
            echo "HitmanContracts.ini: Resolution -> ${RES} (backup: HitmanContracts.ini.bak)"
        else
            echo "HitmanContracts.ini: Resolution line already present — left as-is"
        fi
    else
        # no Resolution line: add one after the ColorDepth line (or at the top)
        cp "$INI" "$INI.bak"
        if grep -qiE '^[[:space:]]*ColorDepth' "$INI"; then
            awk -v res="$RES" 'BEGIN{done=0}
                {print}
                (!done && tolower($1)=="colordepth"){print "Resolution " res; done=1}
                END{if(!done) print "Resolution " res}' "$INI" > "$INI.tmp" \
                && mv "$INI.tmp" "$INI"
        else
            { echo "Resolution ${RES}"; cat "$INI"; } > "$INI.tmp" && mv "$INI.tmp" "$INI"
        fi
        echo "HitmanContracts.ini: added Resolution ${RES} (backup: HitmanContracts.ini.bak)"
    fi

    # Mouse: winemac makes the stock DirectInput mouse misbehave in two ways, both
    # fixed at runtime by the widescreen plugin (all auto under Wine, no ini edit):
    #   - ForceWinMouse  : buttons/firing (DirectInput carries motion but not
    #                      button state on this stack) — kept on the DirectInput
    #                      path so clicks work;
    #   - MouseClipFix   : the mouse-look "edge wall" (a full-desktop cursor clip
    #                      leaves winemac in absolute mode) — inset so it goes
    #                      relative;
    #   - MouseMotionFix : slow-move camera stall (DirectInput's relative axis
    #                      loses sub-pixel motion under winemac) — camera motion is
    #                      fed from the OS cursor while buttons stay on DirectInput.

    # The full-res post-filter patch (PostFilterFullRes=1) needs the game's own
    # post-filter enabled, i.e. PostFilterLOD >= 1 (the stock default is 1). If a
    # previous run set it to 0, restore 1 so the graded/bloom pass runs.
    if grep -qiE '^[[:space:]]*PostFilterLOD[[:space:]]+0' "$INI"; then
        sed "s/^[[:space:]]*[Pp]ostFilterLOD[[:space:]].*/PostFilterLOD 1/" "$INI" > "$INI.tmp" \
            && mv "$INI.tmp" "$INI"
        echo "HitmanContracts.ini: PostFilterLOD -> 1 (so the post-filter runs)"
    fi
fi

# Wine/CrossOver: the game-directory d3d8.dll only loads with the DLL
# override d3d8=native,builtin. Add it to the bottle if we can find CrossOver.
if [ "$(uname -s)" != "Darwin" ]; then
    :  # real Windows: app-dir DLLs win automatically, no override needed
elif [ -n "$WINE" ] || [ -x "/Applications/CrossOver.app/Contents/SharedSupport/CrossOver/bin/wine" ]; then
    CXWINE="${WINE:-/Applications/CrossOver.app/Contents/SharedSupport/CrossOver/bin/wine}"
    # bottle = the dir named ...Bottles/<name>/drive_c/... in the game path
    BOTTLE_NAME="$(printf '%s' "$GAME" | sed -n 's#.*/Bottles/\([^/]*\)/drive_c/.*#\1#p')"
    if [ -n "$BOTTLE_NAME" ]; then
        if CX_BOTTLE="$BOTTLE_NAME" WINEDEBUG=-all "$CXWINE" reg add \
             "HKCU\\Software\\Wine\\DllOverrides" /v d3d8 /d native,builtin /f \
             >/dev/null 2>&1; then
            echo "bottle '$BOTTLE_NAME': set DLL override d3d8=native,builtin"
        else
            echo "WARNING: could not set d3d8=native,builtin automatically."
            echo "  Add it in CrossOver: bottle '$BOTTLE_NAME' > Wine Configuration"
            echo "  > Libraries > new override for 'd3d8' (native, builtin)."
        fi
        # Borderless-fullscreen only hides the macOS menu bar and suppresses
        # the host cursor (near the Dock) when winemac captures the display,
        # which it does for a fullscreen-sized window while the app is active.
        # This bottle-wide Mac Driver option enables that capture without a
        # display mode switch (mode switches misrender under D3DMetal Retina).
        if CX_BOTTLE="$BOTTLE_NAME" WINEDEBUG=-all "$CXWINE" reg add \
             "HKCU\\Software\\Wine\\Mac Driver" /v CaptureDisplaysForFullscreen \
             /d y /f >/dev/null 2>&1; then
            echo "bottle '$BOTTLE_NAME': set Mac Driver CaptureDisplaysForFullscreen=y"
        else
            echo "WARNING: could not set CaptureDisplaysForFullscreen=y automatically."
            echo "  The menu bar / stray cursor fix needs it. Set it by hand:"
            echo "  CX_BOTTLE='$BOTTLE_NAME' wine reg add 'HKCU\\Software\\Wine\\Mac Driver'"
            echo "    /v CaptureDisplaysForFullscreen /d y /f"
        fi
    fi
fi

echo "installed to $GAME"
echo "  loader:     $GAME/d3d8.dll"
echo "  plugin:     $GAME/scripts/HMCWidescreen.asi"
echo "  config:     $GAME/scripts/HMCWidescreen.ini"
echo "  x87 plugin: $GAME/scripts/HMCReducedX87.asi (+ HMCReducedX87/HitmanContracts.exe.x87)"
echo "  profiler:   $GAME/scripts/HMCProfiler.asi (top-right overlay)"
echo "logs after launch: HMCAsiLoader.log, HMCWidescreen.log, HMCReducedX87.log, HMCProfiler.log"
echo "Launch through Steam so the game's Steam check passes."
