/* hmc_display.asi (UI-scale half) — UI scaling for Hitman: Contracts by
 * RE-BELIEVING the engine (ported from the sibling h2sa-asi-plugins).
 *
 * With UIScale=N (>1) the HitmanContracts.ini Resolution is the RENDER
 * resolution R and the engine is made to lay its UI out for L = R/N: the
 * plugin patches the engine's already-parsed copy of the resolution in
 * memory (hmc_uiscale_rebelieve below), so e.g. Resolution 1920x1200 +
 * UIScale=1.5 renders 1920x1200 with the UI sized as at 1280x800. The
 * backbuffer stays pinned to R exactly as with the feature off — the
 * device, the engine's device-derived state and the ModernModes list all
 * keep agreeing — and only the believed-space leftovers are rescaled (the
 * fix_viewport hook below, via the loader's v4 hook chain).
 *
 * Why re-believe and not H2SA's other mode (keep the ini as the layout and
 * GROW the backbuffer behind the engine's back — H2SA's UIScale=-1 auto):
 * that decoupling assumes the engine never looks at the device again, which
 * holds for hitman2.exe (it snapshots the ini at init) but NOT for
 * HitmanContracts.exe — Contracts reads the real device size back after
 * CreateDevice and derives its runtime viewports and post-filter buffers
 * from it, while parts of the 2D layer stay ini-derived, so the UI renders
 * at two inconsistent scales (verified in-game: tiny menu text + oversized
 * background lettering at 1280x800 layout -> 3600x2250 backbuffer).
 * Re-believing inverts the direction: the parsed value everything 2D still
 * reads IS patched, the device is never lied about. UIScale=-1 is therefore
 * NOT supported on Contracts and is treated as off.
 *
 * Timing (differs from H2SA): H2SA's plugins load from inside the engine's
 * LoadLibrary(RenderD3D.dll), after Hitman2.ini is parsed — they scan at
 * plugin load. Contracts imports d3d8.dll statically, so this plugin loads
 * BEFORE the engine parses HitmanContracts.ini; the scan instead runs at
 * the first CreateDevice (widescreen.c's fix_present), on the game thread,
 * after the parse but before the device — and therefore before any
 * device-size copies exist that could false-match.
 *
 * Config lives in [display] (widescreen.c parses and forwards it):
 *   UIScale=0    ; 0/1: off (the default — backbuffer = ini resolution).
 *                ; N>1: render at the ini resolution, UI laid out N x
 *                ; bigger (re-believe above). If the scan finds nothing the
 *                ; feature stays OFF (no H2SA-style supersampling fallback:
 *                ; growing the backbuffer is what breaks on this engine).
 *                ; -1: not supported on Contracts (treated as off).
 */
#include <d3d8.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include "hmc_d3d8.h"
#include "hmc_plugin.h"

static void logf_(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    hmc_vlogf("uiscale", fmt, ap);
    va_end(ap);
}

/* config (set by widescreen.c's parser before the device exists) */
static float g_cfg_uiscale = 0.0f;    /* 0/1 off, -1 auto, >1 explicit */
static unsigned g_patch_mask = ~0u;

/* live state (set at CreateDevice/Reset via hmc_uiscale_setup) */
static int      g_active;
static int      g_ini_w, g_ini_h;     /* believed (layout) resolution */
static unsigned g_bb_w, g_bb_h;       /* real backbuffer */
static double   g_kx = 1.0, g_ky = 1.0;
static int      g_vp_logs;

void hmc_uiscale_config(float uiscale)
{
    g_cfg_uiscale = uiscale;
}

void hmc_uiscale_patchmask(unsigned mask) { g_patch_mask = mask; }

float hmc_uiscale_cfg(void) { return g_cfg_uiscale; }

int hmc_uiscale_wanted(void)
{
    return g_cfg_uiscale < 0.0f || g_cfg_uiscale > 1.0f;
}

/* ky for overlays drawing in real backbuffer pixels (profiler). */
float hmc_uiscale_k(void)
{
    return g_active ? (float)g_ky : 1.0f;
}

/* Hand kx/ky to the loader; it re-exports ky to overlay plugins via
 * HMC_GetUIScale. */
static void publish_to_loader(float kx, float ky)
{
    static void (WINAPI *set)(float, float);
    if (!set) {
        HMODULE ld = GetModuleHandleA("d3d8.dll");
        if (ld)
            set = (void (WINAPI *)(float, float))(uintptr_t)
                GetProcAddress(ld, "HMC_SetUIScale");
    }
    if (set) set(kx, ky);
}

void hmc_uiscale_off(void)
{
    if (g_active) logf_("off");
    g_active = 0;
    publish_to_loader(1.0f, 1.0f);
}

/* Called from widescreen.c's fix_present once the backbuffer size is
 * decided. believed = HitmanContracts.ini resolution, bb = real backbuffer. */
void hmc_uiscale_setup(int ini_w, int ini_h, unsigned bb_w, unsigned bb_h)
{
    if (ini_w < 320 || ini_h < 200 || bb_w < 320 || bb_h < 200) {
        hmc_uiscale_off();
        return;
    }
    g_ini_w = ini_w; g_ini_h = ini_h;
    g_bb_w = bb_w;   g_bb_h = bb_h;
    g_kx = (double)bb_w / (double)ini_w;
    g_ky = (double)bb_h / (double)ini_h;
    g_active = (g_kx > 1.02 || g_ky > 1.02);
    if (!g_active) {
        hmc_uiscale_off();
        return;
    }
    g_vp_logs = 0;
    publish_to_loader((float)g_kx, (float)g_ky);
    logf_("active — layout %dx%d -> backbuffer %ux%u (k=%.4f/%.4f)",
          ini_w, ini_h, bb_w, bb_h, g_kx, g_ky);
}

/* Grow mode needs only believed-space viewport conversion. Keep the loader's
 * per-draw RHW/UV machinery disarmed by publishing 1/1. */
void hmc_uiscale_setup_viewport(int ini_w, int ini_h,
                                unsigned bb_w, unsigned bb_h)
{
    hmc_uiscale_setup(ini_w, ini_h, bb_w, bb_h);
    if (g_active) {
        publish_to_loader(1.0f, 1.0f);
        logf_("viewport-only mode — loader RHW/UV scaling disabled");
    }
}

/* ---- re-believe: patch the engine's already-parsed resolution ---------
 *
 * The parsed HitmanContracts.ini resolution is findable — an adjacent
 * (width,height) pair equal to R, as ints or floats — in the engine's
 * writable memory; patching every copy to L makes the engine lay out for L
 * while the plugin keeps R as the backbuffer. Scanned: the monolithic
 * HitmanContracts.exe image (its .data) plus all committed private RW
 * memory (heap allocations, other threads' stacks). Our own module's
 * globals are MEM_IMAGE of a different module and the region holding the
 * current stack is skipped, so this plugin's copies of R never self-match.
 * Runs at first CreateDevice (see the header) — the device does not exist
 * yet, so no device-derived copies of R can match either. Returns the
 * number of sites patched; 0 = not found (the caller keeps the feature
 * off).
 *
 * The scan steps BYTE-WISE, not in 4-byte strides: the engine's settings
 * object is packed — the ini parse (exe+0x1d886/0x1d8c8) stores the width
 * at object+0x71 and the height at object+0x75, so the live pair sits at
 * unaligned addresses. An aligned scan finds only the engine's stored
 * display-mode list entries (D3DDISPLAYMODE Width/Height on the heap,
 * aligned) — patching those changes nothing about the layout, which is
 * exactly the "3 sites patched, UI unchanged" failure the byte-wise scan
 * fixes. The mode-list entries still match and are patched too; that is
 * cosmetic for the in-game resolution menu, which is locked during
 * re-believe anyway. */
int hmc_uiscale_rebelieve(int rw, int rh, int lw, int lh)
{
    const uint32_t ow = (uint32_t)rw, oh = (uint32_t)rh;
    const uint32_t nw = (uint32_t)lw, nh = (uint32_t)lh;
    const float owf = (float)rw, ohf = (float)rh;
    const float nwf = (float)lw, nhf = (float)lh;
    uint8_t *exe = (uint8_t *)GetModuleHandleA(NULL);
    MEMORY_BASIC_INFORMATION mbi;
    uint8_t *p = (uint8_t *)0x10000;
    int ints = 0, floats = 0, candidates = 0;
    while ((uintptr_t)p < 0x7fff0000u &&
           VirtualQuery(p, &mbi, sizeof(mbi)) == sizeof(mbi)) {
        uint8_t *base = (uint8_t *)mbi.BaseAddress;
        SIZE_T   size = mbi.RegionSize;
        int writable = mbi.State == MEM_COMMIT &&
            !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
            (mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY |
                            PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY));
        int engine_img = exe && (uint8_t *)mbi.AllocationBase == exe;
        int priv = mbi.Type == MEM_PRIVATE;
        int our_stack = (uint8_t *)&mbi >= base && (uint8_t *)&mbi < base + size;
        if (writable && (engine_img || priv) && !our_stack) {
            for (uint8_t *q = base; q + 2 * sizeof(uint32_t) <= base + size;
                 q++) {   /* byte-wise: the settings pair is UNALIGNED */
                uint32_t iv0, iv1;
                memcpy(&iv0, q, 4);
                memcpy(&iv1, q + 4, 4);
                if (iv0 == ow && iv1 == oh) {
                    int site = candidates++;
                    if (!(g_patch_mask & (1u << site))) continue;
                    memcpy(q, &nw, 4);
                    memcpy(q + 4, &nh, 4);
                    if (ints + floats < 8)
                        logf_("re-believe int pair at %p%s%s",
                              q, ((uintptr_t)q & 3) ? " (unaligned)" : "",
                              engine_img ? " (HitmanContracts.exe image)"
                                         : "");
                    ints++;
                    q += 7;   /* past the pair (loop adds 1) */
                } else {
                    float fv0, fv1;
                    memcpy(&fv0, q, 4);
                    memcpy(&fv1, q + 4, 4);
                    if (fv0 == owf && fv1 == ohf) {
                        int site = candidates++;
                        if (!(g_patch_mask & (1u << site))) continue;
                        memcpy(q, &nwf, 4);
                        memcpy(q + 4, &nhf, 4);
                        if (ints + floats < 8)
                            logf_("re-believe float pair at %p%s%s",
                                  q, ((uintptr_t)q & 3) ? " (unaligned)" : "",
                                  engine_img ? " (HitmanContracts.exe image)"
                                             : "");
                        floats++;
                        q += 7;
                    }
                }
            }
        }
        p = base + size;
    }
    logf_("re-believe %dx%d -> %dx%d: %d/%d candidate site(s) patched "
          "(mask=0x%x; %d int + %d float)", rw, rh, lw, lh,
          ints + floats, candidates, g_patch_mask, ints, floats);
    return ints + floats;
}

/* ---- PostFilterLOD force-off ------------------------------------------
 *
 * With the post-filter enabled (PostFilterLOD >= 1) the re-believed engine
 * renders the scene through its post buffers at the LAYOUT resolution and
 * composites it up — soft 3D at best, and the composite topology has
 * repeatedly broken outright. UIScale therefore pairs with PostFilterLOD 0
 * (scene renders full-resolution straight into the backbuffer). An ini
 * edit does NOT stick: the engine re-derives PostFilterLOD from its detail
 * setting and saves it back on every exit. So the live renderer state is
 * patched in memory instead, like the resolution.
 *
 * The chain (from HitmanContracts.exe, read out of the post object's
 * CONSTRUCTOR at exe+0x1dbf7e: `mov eax,[0x79457c]; mov eax,[eax+0x1719];
 * mov [esi+0x18fc],eax`):
 *   manager pointer at exe+0x39457c (VA 0x79457c)
 *   renderer settings object pointer at manager+0x1719 (unaligned; the
 *     post object caches the same pointer at ITS +0x18fc)
 *   PostFilterLOD level as a BYTE at robj+0x135e
 *   derived "post-filter on" flag  as a BYTE at robj+0x13b0
 * The flag derivation at exe+0x1e3a42 computes [robj+0x13b0] from the
 * level bytes at +0x135d/+0x135e (0 when the level is < 1), and the post-
 * buffer allocator at exe+0x1de123 gates on [robj+0x13b0] (with +0x13b1
 * device-support and +0x13ad disable flags alongside). Both the SOURCE
 * level and the DERIVED flag are zeroed, so neither a later re-derivation
 * nor a direct flag consumer can revive the post path. Values are sanity-
 * checked (flag bytes are 0/1; a bigger value means the layout moved in
 * another build — bail rather than corrupt). Called after a successful
 * re-believe and re-asserted once per presented frame, so an in-game
 * detail-setting change cannot re-enable the post path mid-session. */
int hmc_uiscale_force_lod0(void)
{
    static int logs;
    uint8_t *base = (uint8_t *)GetModuleHandleA(NULL);
    if (!base) return 0;
    uint8_t *mgr;
    memcpy(&mgr, base + 0x39457c, sizeof(mgr));
    uint8_t *robj = NULL;
    if (mgr)
        memcpy(&robj, mgr + 0x1719, sizeof(robj));
    if (!robj) {
        /* one-time visibility: a silent bail here previously hid a wrong
         * offset for a whole session */
        static DWORD next_log;
        DWORD now = GetTickCount();
        if (now >= next_log) {
            next_log = now + 10000;
            logf_("post-filter force-off: settings chain not resolved yet "
                  "(mgr=%p robj=%p) — retrying", (void *)mgr, (void *)robj);
        }
        return 0;
    }
    uint8_t lod = robj[0x135e], flag = robj[0x13b0];
    if (lod > 8 || flag > 1) {
        if (logs < 1) {
            logs++;
            logf_("PostFilterLOD fields look wrong (lod=%u flag=%u) — not "
                  "patching (build mismatch?)", lod, flag);
        }
        return 0;
    }
    if (!lod && !flag) return 1;
    robj[0x135e] = 0;
    robj[0x13b0] = 0;
    if (logs < 8) {
        logs++;
        logf_("post-filter forced off (lod %u -> 0, flag %u -> 0; UIScale "
              "renders the scene full-res directly to the backbuffer)",
              lod, flag);
    }
    return 1;
}

/* ---- viewport rescale (fix_viewport hook) -----------------------------
 * The engine sets viewports in believed pixels (full screen, the 3D scene
 * rect, the post-filter buffers' rects...). Anything that fits the believed
 * resolution is scaled to the backbuffer; a viewport that doesn't fit is
 * either garbage (the loader's clamp handles it) or already real-sized
 * (left alone). The rounding here (lround of the scaled edges) matches the
 * loader's post-filter RT rescale exactly, so a scaled full-buffer viewport
 * still fits its scaled render target edge to edge. */
void hmc_uiscale_fix_viewport(D3DVIEWPORT8 *vp, unsigned bbw, unsigned bbh)
{
    (void)bbw; (void)bbh;
    if (!g_active || !vp) return;
    if (vp->X + vp->Width  > (DWORD)g_ini_w + 2 ||
        vp->Y + vp->Height > (DWORD)g_ini_h + 2)
        return;
    DWORD x0 = (DWORD)lround((double)vp->X * g_kx);
    DWORD y0 = (DWORD)lround((double)vp->Y * g_ky);
    DWORD x1 = (DWORD)lround((double)(vp->X + vp->Width)  * g_kx);
    DWORD y1 = (DWORD)lround((double)(vp->Y + vp->Height) * g_ky);
    if (x1 > g_bb_w) x1 = g_bb_w;
    if (y1 > g_bb_h) y1 = g_bb_h;
    if (x1 <= x0 || y1 <= y0) return;
    /* Diagnostic: a scaled viewport that is NOT the full layout rect. The
     * fits-within-layout gate cannot distinguish a layout-space sub-rect
     * (must scale) from a small device-derived sub-view on a full-size
     * canvas (mirror/scope — must not); if such a sub-view ever misrenders,
     * these lines identify the viewport to gate on. */
    if (vp->X || vp->Y ||
        vp->Width + 2 < (DWORD)g_ini_w || vp->Height + 2 < (DWORD)g_ini_h) {
        static int sub_logs;
        if (sub_logs < 8) {
            sub_logs++;
            logf_("viewport SUB-RECT scaled: %lux%lu at %lu,%lu (layout "
                  "%dx%d)", (unsigned long)vp->Width,
                  (unsigned long)vp->Height, (unsigned long)vp->X,
                  (unsigned long)vp->Y, g_ini_w, g_ini_h);
        }
    }
    if (g_vp_logs < 8) {
        g_vp_logs++;
        logf_("viewport %lux%lu at %lu,%lu -> %lux%lu at %lu,%lu",
              (unsigned long)vp->Width, (unsigned long)vp->Height,
              (unsigned long)vp->X, (unsigned long)vp->Y,
              (unsigned long)(x1 - x0), (unsigned long)(y1 - y0),
              (unsigned long)x0, (unsigned long)y0);
    }
    vp->X = x0; vp->Y = y0;
    vp->Width = x1 - x0; vp->Height = y1 - y0;
}
