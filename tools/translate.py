#!/usr/bin/env python3
"""Translate x87 functions of a game module to SSE2; emit a patch file.

Patch file layout (little-endian):
  char  magic[8]  = "HMCONX87"
  u32   version   = 1
  u32   preferred_base
  u32   timedatestamp        PE header stamp, to reject mismatched binaries
  u32   size_of_image
  u32   n_funcs
  u32   blob_total
  char  module[32]           original module file name, lowercase, NUL-padded
  func table [n_funcs] : u32 rva, blob_off, blob_len, fixup_idx, n_fixups
  u32   n_fixups_total
  fixups [n_total]     : u32 blob_off, u32 arg, u32 type
  blob  [blob_total]
"""
import sys, os, struct, argparse
from collections import Counter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from x87.module import Module
from x87 import analysis
from x87.codegen import FuncTranslator, TranslateError
from analyze import find_ftol, find_ci, GAME

import capstone

# Functions other mods patch mid-body: hooking them would make those byte
# patches dead code. On Contracts the widescreen plugin does not byte-patch the
# exe by default (its resolution-snap patch is opportunistic and the potential
# site is a `nofp` function the translator would never select anyway), so there
# is nothing to exclude here. If an on-target build turns out to need a mid-body
# patch protected, add its module basename + function RVA below, or drop the RVA
# into tools/exclusions/<module>.txt.
DEFAULT_EXCLUDE = {
}


def branch_targets(mod):
    """All direct jmp/jcc/call targets module-wide (linear sweep)."""
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.skipdata = True
    targets = set()
    for s, e, data in mod.text:
        for ins in md.disasm(data, s):
            m = ins.mnemonic
            if (m == "call" or m == "jmp" or m in analysis.JCC) and \
                    ins.op_str.startswith("0x"):
                try:
                    targets.add(int(ins.op_str, 16))
                except ValueError:
                    pass
    return targets


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("module", nargs="?", default="HitmanContracts.exe")
    ap.add_argument("--game", default=GAME)
    ap.add_argument("--out", default=None)
    ap.add_argument("--exclude", default="",
                    help="comma-separated function RVAs (hex) to skip")
    ap.add_argument("--diag", action="store_true",
                    help="emit NaN tripwire at float-returning rets")
    ap.add_argument("--pc24", action="store_true",
                    help="round every arithmetic result to float (PC=single)")
    ap.add_argument("--include-all", action="store_true",
                    help="ignore all exclusion lists (testing only)")
    ap.add_argument("--include", default="",
                    help="comma-separated function RVAs (hex) to translate "
                         "even if an exclusion list names them")
    ap.add_argument("--include-start", default="",
                    help="comma-separated function-entry RVAs (hex) to seed "
                         "into discovery (for vtable-only virtuals the "
                         "automatic nets miss on a reloc-stripped image)")
    args = ap.parse_args()

    path = os.path.join(args.game, args.module)
    mod = Module(path)
    print(f"analyzing {args.module} ...")

    # Seed entries that discovery can't reach on its own. On a reloc-stripped
    # image (HitmanContracts.exe) the vtable/function-pointer net is dead, so
    # virtual methods with a non-standard prologue are never offered to the
    # translator — e.g. the rain particle builder at 0x1d9a90. Sourced from the
    # CLI plus an optional starts/<module>.txt companion to exclusions/.
    start_rvas = {int(x, 16) for x in args.include_start.split(",") if x}
    starts_file = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                               "starts",
                               os.path.basename(path).lower() + ".txt")
    if os.path.exists(starts_file):
        with open(starts_file) as fh:
            for line in fh:
                line = line.split("#")[0].strip()
                if line:
                    start_rvas.add(int(line, 16))
        print(f"loaded {starts_file}")
    extra_starts = {mod.base + r for r in start_rvas}
    if start_rvas:
        print(f"seeded starts: {sorted(hex(r) for r in start_rvas)}")

    _, funcs0 = analysis.analyze_module(mod, extra_starts=extra_starts)
    ftol = set(find_ftol(mod, funcs0))
    ci = find_ci(mod, funcs0)
    an, funcs = analysis.analyze_full(mod, ftol, ci, extra_starts=extra_starts)

    # never hook the CRT helpers themselves (their callers are rewritten)
    helper_vas = ftol | set(ci)
    excl_rvas = {int(x, 16) for x in args.exclude.split(",") if x}
    if not args.include_all:
        excl_rvas |= DEFAULT_EXCLUDE.get(os.path.basename(path).lower(), set())
        excl_file = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                 "exclusions",
                                 os.path.basename(path).lower() + ".txt")
        if os.path.exists(excl_file):
            with open(excl_file) as fh:
                for line in fh:
                    line = line.split("#")[0].strip()
                    if line:
                        excl_rvas.add(int(line, 16))
            print(f"loaded {excl_file}")
    incl_rvas = {int(x, 16) for x in args.include.split(",") if x}
    if incl_rvas:
        print(f"exclusion override: {sorted(hex(r) for r in incl_rvas)}")
        excl_rvas -= incl_rvas

    # safety: don't hook functions whose first 5 bytes are a branch target
    tgts = branch_targets(mod)
    ok = [f for f in funcs if f.status == "ok" and f.start not in helper_vas
          and (f.start - mod.base) not in excl_rvas]
    entry_hazard = [f for f in ok
                    if any(f.start < t < f.start + 5 for t in tgts)]
    hz = {f.start for f in entry_hazard}
    ok = [f for f in ok if f.start not in hz]
    print(f"translatable: {len(ok)} functions "
          f"({len(entry_hazard)} dropped: entry branch hazard)")

    blob_parts = []
    func_recs = []
    fixup_recs = []
    orig_parts = []       # first 5 bytes of each translated function entry
    blob_off = 0
    fails = Counter()
    n_x87_done = 0
    for f in ok:
        try:
            tr = FuncTranslator(mod, f, ftol, ci, diag=args.diag,
                                pc24=args.pc24)
            blob, fixups = tr.run()
        except (TranslateError, ValueError) as e:
            fails[str(e)] += 1
            continue
        func_recs.append((f.start - mod.base, blob_off, len(blob),
                          len(fixup_recs), len(fixups)))
        for off, typ, arg in fixups:
            if typ == 6:  # ABS32_BLOB: arg is function-local, make it global
                arg += blob_off
            fixup_recs.append((blob_off + off, arg & 0xFFFFFFFF, typ))
        blob_parts.append(blob)
        orig_parts.append(mod.read(f.start, 5))
        blob_off += len(blob)
        n_x87_done += f.n_x87

    blob = b"".join(blob_parts)
    print(f"translated {len(func_recs)} functions, {n_x87_done} x87 insns, "
          f"blob {len(blob)//1024} KB, {len(fixup_recs)} fixups")
    if fails:
        print("codegen failures:")
        for k, n in fails.most_common(10):
            print(f"  {k}: {n}")

    name = os.path.basename(path).lower().encode()[:31]
    out = args.out or os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "..", "dist",
        os.path.basename(path) + ".x87")
    with open(out, "wb") as fh:
        fh.write(b"HMCONX87")
        fh.write(struct.pack(
            "<IIIIII", 1, mod.base,
            mod.pe.FILE_HEADER.TimeDateStamp,
            mod.pe.OPTIONAL_HEADER.SizeOfImage,
            len(func_recs), len(blob)))
        fh.write(name + b"\x00" * (32 - len(name)))
        for rec in func_recs:
            fh.write(struct.pack("<IIIII", *rec))
        fh.write(struct.pack("<I", len(fixup_recs)))
        for rec in fixup_recs:
            fh.write(struct.pack("<III", *rec))
        fh.write(blob)
        # Optional trailing section: original entry bytes (5 per func, same
        # order as the func table). Lets the runtime loader gate hooking on
        # unpack completion for packed modules (HitmanContracts.exe). Readers that
        # stop at blob_total ignore it, so the format stays backward-compat.
        orig = b"".join(orig_parts)
        assert len(orig) == len(func_recs) * 5
        fh.write(orig)
    print(f"wrote {out} ({os.path.getsize(out)//1024} KB)")


if __name__ == "__main__":
    main()
