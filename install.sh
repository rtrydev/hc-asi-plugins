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

# Pre-rename artifact/config/log names (one ASI per feature, CamelCase); an
# install over an old layout migrates the config and removes these.
remove_legacy() {
    rm -f "$GAME/scripts/HMCAsiLoader.log"
    rm -f "$GAME/scripts/HMCWidescreen.asi"
    rm -f "$GAME/scripts/HMCWidescreen.log"
    rm -f "$GAME/scripts/HMCReducedX87.asi"
    rm -f "$GAME/scripts/HMCReducedX87.log"
    rm -rf "$GAME/scripts/HMCReducedX87"
    rm -f "$GAME/scripts/HMCProfiler.asi"
    rm -f "$GAME/scripts/HMCProfiler.log"
    rm -f "$GAME/scripts/HMCReducedX87-diag.asi"
}

if [ "$1" = "-u" ]; then
    rm -f "$GAME/d3d8.dll"
    rm -f "$GAME/scripts/hmc_asi_loader.log"
    rm -f "$GAME/scripts/hmc_display.asi"
    rm -f "$GAME/scripts/hmc_display.log"
    rm -f "$GAME/scripts/hmc_reduced_x87.asi"
    rm -f "$GAME/scripts/hmc_reduced_x87_diag.asi"
    rm -f "$GAME/scripts/hmc_reduced_x87.log"
    rm -rf "$GAME/scripts/hmc_reduced_x87"
    remove_legacy
    # the plugin .ini is user config; left in place on purpose.
    # HitmanContracts.ini is not touched on uninstall; restore
    # HitmanContracts.ini.bak by hand to undo the Resolution line.
    echo "uninstalled (d3d8.dll + plugins removed; HitmanContracts.ini left as-is)"
    exit 0
fi

[ -f "$HERE/dist/d3d8.dll" ] || { echo "build first: (cd runtime && make)"; exit 1; }
[ -f "$HERE/dist/hmc_display.asi" ] || { echo "build first: (cd runtime && make)"; exit 1; }
[ -d "$GAME" ] || { echo "game dir not found: $GAME (set HMC_GAME_DIR)"; exit 1; }

mkdir -p "$GAME/scripts"

# ASI loader: d3d8.dll proxy in the game root (also carries the D3D8 hooks)
cp "$HERE/dist/d3d8.dll" "$GAME/d3d8.dll"

# Display plugin: widescreen + startup fix + mouse/pacing fixes + the
# profiler overlay, one ASI. Config: hmc_display.ini, sections [display]
# (widescreen/startup keys, also read by the d3d8.dll loader) and [profiler].
cp "$HERE/dist/hmc_display.asi" "$GAME/scripts/"
if [ ! -f "$GAME/scripts/hmc_display.ini" ]; then
    if [ -f "$GAME/scripts/HMCWidescreen.ini" ]; then
        # migrate the pre-rename per-plugin configs, keys preserved as-is
        {
            echo "[display]"
            grep -v '^\[' "$GAME/scripts/HMCWidescreen.ini"
            echo ""
            echo "[profiler]"
            if [ -f "$GAME/scripts/HMCProfiler.ini" ]; then
                grep -v '^\[' "$GAME/scripts/HMCProfiler.ini"
            else
                printf 'Enabled=1\nScale=1.0\nShowCPU=0\nOffsetX=8\nOffsetY=8\n'
            fi
        } > "$GAME/scripts/hmc_display.ini"
        echo "migrated HMCWidescreen.ini/HMCProfiler.ini -> hmc_display.ini"
    else
        # ShowCPU=0 by default: the CPU-share breakdown suspends the game's
        # main thread to sample it, which costs frame time — set ShowCPU=1
        # only when you want the X87/GAME/REST diagnostic.
        printf '[display]\nEnabled=1\nFullscreen=0\nBorderless=-1\nFOVCorrect=1\nFOVFactor=1.0\nPreserveAspect=1\nModernModes=1\nCursorFix=0\nFpsCap=60\nVSync=-1\nBackBuffers=2\nPostFilterFullRes=1\nPostFilterAlphaFix=1\nPostFilterOpaqueRT=1\nPostFilterOpaqueRTMask=3\nRainEmitCap=192\nRainSystemCap=32\nForceWinMouse=-1\nMouseClipFix=-1\nMouseMotionFix=-1\nUIScale=0\nUIScalePostFilter=1\nUIScaleStrict2D=1\n\n[profiler]\nEnabled=1\nScale=1.0\nShowCPU=0\nOffsetX=8\nOffsetY=8\n' \
            > "$GAME/scripts/hmc_display.ini"
    fi
fi
if ! grep -q '^[[:space:]]*UIScale' "$GAME/scripts/hmc_display.ini"; then
    # Surface UIScale, off by default. Recommended: -N (N>1) treats the game
    # Resolution as UI layout size and grows the render backbuffer N x.
    awk '{ print } /^\[display\]/ || /^\[widescreen\]/ { print "UIScale=0" }' \
        "$GAME/scripts/hmc_display.ini" > "$GAME/scripts/hmc_display.ini.tmp" \
        && mv "$GAME/scripts/hmc_display.ini.tmp" "$GAME/scripts/hmc_display.ini"
    echo "hmc_display.ini: added UIScale=0 to [display]"
fi
rm -f "$GAME/scripts/HMCWidescreen.ini" "$GAME/scripts/HMCProfiler.ini"

# Reduced-precision x87 plugin: the SSE2-translated HitmanContracts.exe blob for
# CrossOver/Rosetta 2 performance. The .asi loads any <module>.x87 blob from
# scripts/hmc_reduced_x87/. Contracts is a single unpacked exe, so there is just
# one blob (HitmanContracts.exe.x87) — no separate renderer DLL, no dump step.
if [ -f "$HERE/dist/hmc_reduced_x87.asi" ] && [ -f "$HERE/dist/HitmanContracts.exe.x87" ]; then
    cp "$HERE/dist/hmc_reduced_x87.asi" "$GAME/scripts/"
    mkdir -p "$GAME/scripts/hmc_reduced_x87"
    cp "$HERE/dist/HitmanContracts.exe.x87" "$GAME/scripts/hmc_reduced_x87/"
    echo "x87 plugin: hmc_reduced_x87.asi + HitmanContracts.exe.x87 installed"
else
    echo "NOTE: x87 plugin not installed (run tools/translate.py, then rebuild)"
fi

# Drop any pre-rename plugin files so the loader does not load a feature twice.
remove_legacy

# Give widescreen out of the box. HitmanContracts.ini has no "Resolution" line
# by default (the engine falls back to a built-in default), and the display
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
    # fixed at runtime by the display plugin (all auto under Wine, no ini edit):
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
    # previous run set it to 0, restore 1 so the graded/bloom pass runs. (The
    # post-filter also runs under UIScale: the loader rescales its
    # believed-space viewports/quads/UVs; UIScalePostFilter=0 is the opt-out.)
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
echo "  loader:      $GAME/d3d8.dll"
echo "  display:     $GAME/scripts/hmc_display.asi (widescreen + profiler overlay)"
echo "  config:      $GAME/scripts/hmc_display.ini"
echo "  x87 plugin:  $GAME/scripts/hmc_reduced_x87.asi (+ hmc_reduced_x87/HitmanContracts.exe.x87)"
echo "logs after launch: hmc_asi_loader.log, hmc_display.log, hmc_reduced_x87.log"
echo "Launch through Steam so the game's Steam check passes."
