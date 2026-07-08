#!/bin/bash
# Differential tester for the HitmanContracts.exe x87 -> SSE2 translation.
#
# Builds difftest.exe, stages it plus the .x87 blob and the leaf-function
# manifest into the game directory, and runs it inside the CrossOver Steam
# bottle. difftest loads HitmanContracts.exe itself (with DONT_RESOLVE_DLL_
# REFERENCES — no entry point run, no imports resolved; the exe is used purely
# as a code image), maps the translated blob with fixups applied, then calls
# every leaf function both ways with identical randomized register/stack/memory
# contexts and compares eax/edx/st0 and touched memory.
#
# Contracts is monolithic and NOT packed, so the module-under-test is the real
# HitmanContracts.exe already sitting in the game directory — we do NOT copy or
# delete it (only the blob + manifest + difftest.exe are staged/cleaned).
#
# Prereqs: (cd runtime && make) and python3 tools/{translate,gen_manifest}.py
# have produced dist/HitmanContracts.exe.x87 and dist/HitmanContracts.exe.leaf.txt.
#
# Args: [maxfuncs] [rich]  — forwarded to difftest.exe (default: all funcs).
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
WINE="${WINE:-/Applications/CrossOver.app/Contents/SharedSupport/CrossOver/bin/wine}"
export CX_BOTTLE="${CX_BOTTLE:-Steam}"
export WINEDEBUG=-all

GAME="${HMC_GAME_DIR:-$HOME/Library/Application Support/CrossOver/Bottles/Steam/drive_c/Program Files (x86)/Steam/steamapps/common/Hitman Contracts}"
GAME_WIN='C:\Program Files (x86)\Steam\steamapps\common\Hitman Contracts'

[ -f "$ROOT/dist/HitmanContracts.exe.x87" ] || { echo "run tools/translate.py first"; exit 1; }
[ -f "$ROOT/dist/HitmanContracts.exe.leaf.txt" ] || { echo "run tools/gen_manifest.py first"; exit 1; }

i686-w64-mingw32-gcc -O1 -Wall -msse2 -static -static-libgcc \
    -o "$HERE/difftest.exe" "$HERE/difftest.c" "$HERE/probe.S" \
    "$ROOT/runtime/helpers.S"

# Stage only the tester + blob + manifest. HitmanContracts.exe is already there
# (the real game file) — never copy or remove it.
cp "$HERE/difftest.exe" "$ROOT/dist/HitmanContracts.exe.x87" \
   "$ROOT/dist/HitmanContracts.exe.leaf.txt" "$GAME/"
trap 'rm -f "$GAME/difftest.exe" "$GAME/HitmanContracts.exe.x87" "$GAME/HitmanContracts.exe.leaf.txt"' EXIT

"$WINE" --bottle "$CX_BOTTLE" --workdir "$GAME_WIN" \
    --cx-app "$GAME_WIN\\difftest.exe" \
    HitmanContracts.exe HitmanContracts.exe.x87 HitmanContracts.exe.leaf.txt "$@" 2>&1 \
    | grep -vE 'fixme|err:|warn:|wine:' || true
