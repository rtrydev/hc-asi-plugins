/* HMC ASI Loader — a d3d8.dll proxy that loads every .asi from the
 * scripts directory and gives those plugins a stable hook onto the game's
 * Direct3D 8 interface (32-bit, built with mingw like the plugins).
 *
 * Why d3d8.dll (and not dsound.dll as in the Codename 47 build): Hitman:
 * Contracts is a Direct3D 8 title. Unlike Hitman 2 — which loads a separate
 * RenderD3D.dll at run time — Contracts is a single monolithic executable
 * (the RenderD3D renderer, SoundEngine and SDL_Engine are all statically
 * linked into HitmanContracts.exe), and it imports Direct3DCreate8 from
 * d3d8.dll in its own import table. So a d3d8.dll proxy in the game
 * directory is mapped at process start, before the game ever creates its
 * device — the ideal place to intervene.
 *
 * On process attach it:
 *  - loads every *.asi from the scripts/ directory next to it, logging to
 *    scripts/HMCAsiLoader.log;
 *  - exports all five real d3d8.dll entry points at their real ordinals
 *    (see d3d8.def) and forwards four of them to the system d3d8.dll,
 *    resolved lazily on first call;
 *  - wraps the fifth, Direct3DCreate8, so the returned IDirect3D8 —and the
 *    IDirect3DDevice8 it later creates— have their CreateDevice / Reset /
 *    SetTransform vtable slots redirected to this module. Plugins that
 *    registered via HMC_RegisterD3D8Hooks then get to rewrite the
 *    presentation parameters and the projection matrix. The D3D8 vtable
 *    layout is a fixed COM ABI, so this needs no game-build offsets.
 *
 * Under Wine/CrossOver the bottle needs the DLL override
 * "d3d8=native,builtin" so the game-directory proxy is picked over the
 * builtin d3d8 (which itself is what we forward to).
 */
#include <d3d8.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "hmc_d3d8.h"

/* Confirmed IDirect3D8 / IDirect3DDevice8 vtable slot indices (fixed D3D8
 * COM ABI, verified against the mingw d3d8.h vtable structs). */
#define IDX_D3D8_GETMODECOUNT   6
#define IDX_D3D8_ENUMMODES      7
#define IDX_D3D8_CREATEDEVICE   15
#define IDX_DEV_RESET           14
#define IDX_DEV_PRESENT         15
#define IDX_DEV_CREATEVB        23
#define IDX_DEV_CREATETEXTURE   20
#define IDX_DEV_CREATERENDERTGT 25
#define IDX_DEV_CREATEIMAGESURF 27
#define IDX_DEV_SETRENDERTARGET 31
#define IDX_DEV_SETTRANSFORM    37
#define IDX_DEV_SETVIEWPORT     40
#define IDX_DEV_SETRENDERSTATE  50
#define IDX_DEV_SETTEXTURE      61
#define IDX_DEV_DRAWPRIM        70
#define IDX_DEV_DRAWINDEXPRIM   71
#define IDX_DEV_DRAWPRIMUP      72
#define IDX_DEV_DRAWINDEXPRIMUP 73
/* IDirect3DVertexBuffer8: IUnknown(0..2) + IDirect3DResource8(3..10) + Lock(11) */
#define IDX_VB_LOCK             11

static FILE *g_log;
static char g_dir[MAX_PATH];        /* game directory (location of this dll) */
static HINSTANCE g_self;
static HMODULE g_real;              /* system d3d8.dll, resolved lazily */

#define MAX_HOOKSETS 8
static HMCD3D8Hooks g_hooks[MAX_HOOKSETS];  /* one per registered plugin */
static int g_n_hooks;               /* number of registered hook sets */
static void *g_orig_createdevice;
static void *g_orig_reset;
static void *g_orig_present;
static void *g_orig_settransform;
static void *g_orig_setviewport;
static void *g_orig_createtexture;
static void *g_orig_createrendertgt;
static void *g_orig_createimagesurf;
static void *g_orig_setrendertarget;
static void *g_orig_setrenderstate;
static void *g_orig_settexture;
static unsigned int g_bbw, g_bbh;   /* backbuffer size of the live device */
static int g_vp_logs, g_proj_logs;  /* diagnostic sample counters */
static int g_rt_logs;               /* render-target creation log counter */

/* Post-filter alpha repair. On the CrossOver D3D->Metal stack the A8
 * render-target alpha that fixes Contracts's ground/detail blending also
 * leaves the game's post-filter composite at alpha ~= 0, so its final
 * SRCALPHA blend contributes nothing. Opt-in only: when enabled, draws that
 * blend a render-target texture back to the backbuffer with SRCBLEND=SRCALPHA
 * are submitted with SRCBLEND=ONE, then the logical game state is restored.
 * Ordinary alpha-blended geometry/HUD textures are not render-target textures,
 * so they do not match this gate. */
static int g_postfx_alpha_fix;
static int g_postfx_opaque_rt;
/* Which of the six post-filter RT-creation sites PostFilterOpaqueRT converts to
 * X8R8G8B8 (bit i <-> g_postfx_rt_sites[i]). Default 0x03 = the quarter-res
 * bloom-blur buffer (site 0) + the full-res scene copy the bloom bright-pass
 * samples (site 1); those are the two the bright-pass multiplies by alpha and
 * that go black on D3DMetal without opaque alpha. The remaining full-res buffers
 * (sites 2-5) keep their A8 alpha — one of them is the colour-grade target the
 * dying/slow-mo B&W desaturation ramps through its DESTINATION ALPHA, and
 * forcing THAT one opaque makes the death grade degenerate and stalls the frame
 * on D3DMetal. `PostFilterOpaqueRTMask` (decimal, 0..63) overrides the set. */
static unsigned g_postfx_opaque_mask = 0x03;
static IDirect3DSurface8 *g_backbuffer;
static int g_rt_is_backbuffer = 1;
static int g_stage0_is_rttex;
static UINT g_stage0_rt_w, g_stage0_rt_h;
static D3DFORMAT g_stage0_rt_fmt;
static int g_alpha_blend;
static DWORD g_srcblend = D3DBLEND_ONE;
static unsigned int g_postfx_alpha_hits;
static unsigned int g_postfx_probe_logs;

/* The six post-filter RT-creation call sites (return addresses), in creation
 * order, with the RenderD3D post-object member slot each buffer lands in. Site 0
 * is the quarter-res 480x270 bloom-blur buffer; sites 1-5 are full-res
 * 1920x1080 buffers (scene copy / colour-grade / bloom source). */
static const struct { uint32_t rva; uint32_t slot; } g_postfx_rt_sites[6] = {
    { 0x1de1a0, 0x18ec },  /* 0: quarter-res 480x270 bloom-blur */
    { 0x1de1c8, 0x189c },  /* 1: full-res */
    { 0x1de1f0, 0x18a0 },  /* 2: full-res */
    { 0x1de218, 0x18a4 },  /* 3: full-res */
    { 0x1de23f, 0x18e8 },  /* 4: full-res */
    { 0x1de263, 0x18f4 },  /* 5: full-res */
};

/* Return the site index (0..5) whose RT conversion is enabled by the mask for
 * this caller, or -1 if this caller is not a converted post-filter site. */
static int postfilter_rt_site(uint32_t rva)
{
    for (int i = 0; i < 6; i++)
        if (g_postfx_rt_sites[i].rva == rva)
            return (g_postfx_opaque_mask & (1u << i)) ? i : -1;
    return -1;
}

#define MAX_RT_TEX 64
typedef struct { IDirect3DBaseTexture8 *tex; UINT w, h; D3DFORMAT fmt; } RTTex;
static RTTex g_rt_tex[MAX_RT_TEX];
static int g_n_rt_tex;

/* ---- draw/lock diagnostic (HMC_DRAWSTATS=1) --------------------------
 * Opt-in per-frame instrumentation to settle whether a heavy scene (e.g.
 * rain) is CPU/dynamic-VB-lock bound or GPU/fill bound. Every window of
 * DRAWSTATS_WINDOW frames it logs, to HMCAsiLoader.log, the window's fps and
 * per-frame draw count, VB-lock count (and the DISCARD subset), time spent
 * inside VB Lock, and time spent inside Present. The fps of each line is
 * self-correlating: rainy stretches show up as the low-fps lines, so a
 * side-by-side read of a rainy vs. clear capture answers A (locks/locktime
 * jump with fps) vs. B (locks flat, present time grows). Off => none of the
 * hot-path slots are wrapped, so play is untouched. */
#define DRAWSTATS_WINDOW 60
static int g_drawstats;               /* HMC_DRAWSTATS=1 enables the probe */
static void *g_orig_createvb;
static void *g_orig_vblock;
static void *g_orig_drawprim;
static void *g_orig_drawindexprim;
static void *g_orig_drawprimup;
static void *g_orig_drawindexprimup;
static int g_vb_lock_patched;
static LARGE_INTEGER g_qpf;           /* QPC frequency, queried once */
static LARGE_INTEGER g_last_frame_end;/* QPC at the previous Present return */
/* per-frame accumulators, folded into the window at each Present */
static unsigned int g_f_draws, g_f_vblocks, g_f_discards;
static unsigned int g_f_dip, g_f_up;   /* indexed vs user-pointer draw split */
static LONGLONG g_f_lock_ticks, g_f_draw_ticks;
/* per-window accumulators */
static int g_win_frames;
static double g_win_ms_sum, g_win_ms_max;
static unsigned int g_win_draws, g_win_vblocks, g_win_discards;
static unsigned int g_win_dip, g_win_up;
static LONGLONG g_win_lock_ticks, g_win_present_ticks, g_win_draw_ticks;

#define MAX_DRAW_SITES 64
typedef struct { uint32_t rva; unsigned int count, up; } DrawSite;
static DrawSite g_draw_sites[MAX_DRAW_SITES];
static int g_n_draw_sites;
static unsigned int g_draw_site_total;

/* Rain-system probe: the rain particle renderer (exe+0x1d9a90) calls the
 * transform helper exe+0x21d750 exactly once per rain "system"/layer, at the
 * single call site exe+0x1da403. Redirecting only that one call site to a tiny
 * counter stub reveals how many rain systems are drawn per frame — the datum
 * needed to decide whether cutting whole layers is viable. 0x21d750 has 279
 * callers module-wide; we touch none of them, only this call. */
#define RAIN_CALLSITE_RVA 0x1da403
#define RAIN_HELPER_RVA   0x21d750
static volatile unsigned int g_rain_sys_frame;  /* reset each Present */
static unsigned int g_win_rainsys;
static unsigned char *g_rain_cave;

/* Experimental rain CPU limiter. The profiler shows worst-case rain time in
 * the per-particle vertex builder at exe+0x1daa00..0x1dadff. Just before that
 * loop, the game stores the number of rain quads to emit at [esp+0x40]. When
 * RainEmitCap is set, a tiny runtime patch clamps that count before the loop
 * starts. This keeps the renderer/state path intact and only limits bursty
 * geometry generation. */
#define RAIN_EMITCAP_RVA 0x1da93a
static int g_rain_emit_cap;
static volatile unsigned int g_rain_cap_frame;
static unsigned int g_win_raincaps;
static unsigned char *g_rain_cap_cave;

/* A lower-level cap for the same renderer: after the game has decided a rain
 * system is visible but before it enters the expensive particle builder, limit
 * how many visible systems are processed in the current frame. This addresses
 * the common 35-45fps case where many systems each stay under RainEmitCap. */
#define RAIN_VISIBLE_GATE_RVA 0x1da4c0
#define RAIN_VISIBLE_CONT_RVA 0x1da4c6
#define RAIN_VISIBLE_SKIP_RVA 0x1dae06
static int g_rain_system_cap;
static volatile unsigned int g_rain_visible_frame;
static volatile unsigned int g_rain_sysskip_frame;
static unsigned int g_win_rainvisible, g_win_rainsysskip;
static unsigned char *g_rain_system_cave;

static void logf_(const char *fmt, ...)
{
    if (!g_log) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

typedef ULONG (WINAPI *rel_t)(void *);

static void release_iunknown(void *p)
{
    if (p) ((rel_t)(*(void ***)p)[2])(p);
}

static void remember_rt_texture(IDirect3DBaseTexture8 *tex, UINT w, UINT h,
                                D3DFORMAT fmt)
{
    if (!tex) return;
    for (int i = 0; i < g_n_rt_tex; i++) {
        if (g_rt_tex[i].tex == tex) {
            g_rt_tex[i].w = w; g_rt_tex[i].h = h; g_rt_tex[i].fmt = fmt;
            return;
        }
    }
    int i = g_n_rt_tex < MAX_RT_TEX ? g_n_rt_tex++ : 0;
    g_rt_tex[i].tex = tex;
    g_rt_tex[i].w = w;
    g_rt_tex[i].h = h;
    g_rt_tex[i].fmt = fmt;
}

static RTTex *find_rt_texture(IDirect3DBaseTexture8 *tex)
{
    if (!tex) return NULL;
    for (int i = 0; i < g_n_rt_tex; i++)
        if (g_rt_tex[i].tex == tex) return &g_rt_tex[i];
    return NULL;
}

static void capture_backbuffer(IDirect3DDevice8 *dev)
{
    typedef HRESULT (WINAPI *gb_t)(IDirect3DDevice8 *, UINT,
        D3DBACKBUFFER_TYPE, IDirect3DSurface8 **);
    if (g_backbuffer) {
        release_iunknown(g_backbuffer);
        g_backbuffer = NULL;
    }
    if (dev)
        ((gb_t)(*(void ***)dev)[16])(dev, 0, D3DBACKBUFFER_TYPE_MONO,
                                     &g_backbuffer);
    g_rt_is_backbuffer = 1;
}

static int read_postfx_alpha_ini(void)
{
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s\\scripts\\HMCWidescreen.ini", g_dir);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[128];
    int enabled = 0, b;
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, " PostFilterAlphaFix = %d", &b) == 1 ||
            sscanf(line, " PostFilterAlphaFix=%d", &b) == 1) {
            enabled = b != 0;
            break;
        }
    }
    fclose(f);
    return enabled;
}

static int read_int_ini(const char *name, int def)
{
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s\\scripts\\HMCWidescreen.ini", g_dir);
    FILE *f = fopen(path, "r");
    if (!f) return def;
    char line[128], lhs[64];
    int value = def;
    while (fgets(line, sizeof(line), f)) {
        int v;
        lhs[0] = 0;
        if ((sscanf(line, " %63[^= ] = %d", lhs, &v) == 2 ||
             sscanf(line, " %63s %d", lhs, &v) == 2) &&
            _stricmp(lhs, name) == 0) {
            value = v;
            break;
        }
    }
    fclose(f);
    return value;
}

static FARPROC resolve(const char *name)
{
    if (!g_real) {
        char path[MAX_PATH];
        UINT n = GetSystemDirectoryA(path, MAX_PATH - 12);
        if (n == 0 || n >= MAX_PATH - 12) return NULL;
        strcat(path, "\\d3d8.dll");
        HMODULE h = LoadLibraryA(path);
        if (!h || h == (HMODULE)g_self) {
            logf_("system d3d8.dll unavailable (h=%p self=%p) — proxy "
                  "exports will fail", h, g_self);
            g_real = (HMODULE)(uintptr_t)-1;
        } else {
            g_real = h;
            logf_("forwarding to system d3d8.dll at %p", h);
        }
    }
    if (g_real == (HMODULE)(uintptr_t)-1) return NULL;
    FARPROC f = GetProcAddress(g_real, name);
    if (!f) logf_("system d3d8.dll lacks %s", name);
    return f;
}

/* Overwrite one COM vtable slot in place. There is a single IDirect3D8 and
 * a single device for the process, so patching the shared vtable slot is
 * fine; the original is saved on first patch. */
static void patch_slot(void *iface, int idx, void *newfn, void **saved)
{
    void **vtbl = *(void ***)iface;
    if (!*saved) *saved = vtbl[idx];
    DWORD old;
    if (VirtualProtect(&vtbl[idx], sizeof(void *), PAGE_READWRITE, &old)) {
        vtbl[idx] = newfn;
        VirtualProtect(&vtbl[idx], sizeof(void *), old, &old);
    }
}

/* ---- wrapped D3D8 methods -------------------------------------------- */

/* Diagnostic: log render-target/texture/image-surface creations with their
 * size, usage and the CALLER's return address, so the game code that computes a
 * given surface size (e.g. the half-resolution post-filter buffer) can be
 * located precisely. Only render-target-usage surfaces are interesting. */
static HRESULT WINAPI hook_CreateTexture(IDirect3DDevice8 *self, UINT W, UINT H,
    UINT Levels, DWORD Usage, D3DFORMAT Fmt, D3DPOOL Pool,
    IDirect3DTexture8 **pp)
{
    typedef HRESULT (WINAPI *ct_t)(IDirect3DDevice8 *, UINT, UINT, UINT, DWORD,
        D3DFORMAT, D3DPOOL, IDirect3DTexture8 **);
    uint8_t *base = (uint8_t *)GetModuleHandleA(NULL);
    void *ret = __builtin_return_address(0);
    uint32_t rva = base ? (uint32_t)((uint8_t *)ret - base) : 0;
    D3DFORMAT requested_fmt = Fmt;
    if (g_postfx_opaque_rt && (Usage & D3DUSAGE_RENDERTARGET) &&
        Fmt == D3DFMT_A8R8G8B8) {
        int site = postfilter_rt_site(rva);
        if (site >= 0) {
            Fmt = D3DFMT_X8R8G8B8;
            if (g_rt_logs < 40)
                logf_("PostFilterOpaqueRT: site%d slot 0x%x CreateTexture RT "
                      "exe+0x%x %ux%u fmt %d -> %d", site,
                      g_postfx_rt_sites[site].slot, rva, W, H, requested_fmt,
                      Fmt);
        }
    }
    if ((Usage & D3DUSAGE_RENDERTARGET) && g_rt_logs < 40) {
        g_rt_logs++;
        logf_("CreateTexture RT %ux%u fmt=%d usage=0x%lx caller=%p (exe+0x%tx)",
              W, H, Fmt, (unsigned long)Usage, ret,
              base ? (uint8_t *)ret - base : 0);
    }
    HRESULT hr = ((ct_t)g_orig_createtexture)(self, W, H, Levels, Usage, Fmt,
                                              Pool, pp);
    if (SUCCEEDED(hr) && pp && *pp && (Usage & D3DUSAGE_RENDERTARGET))
        remember_rt_texture((IDirect3DBaseTexture8 *)*pp, W, H, Fmt);
    return hr;
}

static HRESULT WINAPI hook_CreateRenderTarget(IDirect3DDevice8 *self, UINT W,
    UINT H, D3DFORMAT Fmt, D3DMULTISAMPLE_TYPE MS, BOOL Lockable,
    IDirect3DSurface8 **pp)
{
    typedef HRESULT (WINAPI *cr_t)(IDirect3DDevice8 *, UINT, UINT, D3DFORMAT,
        D3DMULTISAMPLE_TYPE, BOOL, IDirect3DSurface8 **);
    if (g_rt_logs < 40) {
        g_rt_logs++;
        logf_("CreateRenderTarget %ux%u fmt=%d caller=%p (exe+0x%tx)",
              W, H, Fmt, __builtin_return_address(0),
              (uint8_t *)__builtin_return_address(0) -
              (uint8_t *)GetModuleHandleA(NULL));
    }
    return ((cr_t)g_orig_createrendertgt)(self, W, H, Fmt, MS, Lockable, pp);
}

static HRESULT WINAPI hook_CreateImageSurface(IDirect3DDevice8 *self, UINT W,
    UINT H, D3DFORMAT Fmt, IDirect3DSurface8 **pp)
{
    typedef HRESULT (WINAPI *ci_t)(IDirect3DDevice8 *, UINT, UINT, D3DFORMAT,
        IDirect3DSurface8 **);
    if (g_rt_logs < 40) {
        g_rt_logs++;
        logf_("CreateImageSurface %ux%u fmt=%d caller=%p (exe+0x%tx)",
              W, H, Fmt, __builtin_return_address(0),
              (uint8_t *)__builtin_return_address(0) -
              (uint8_t *)GetModuleHandleA(NULL));
    }
    return ((ci_t)g_orig_createimagesurf)(self, W, H, Fmt, pp);
}

static HRESULT WINAPI hook_SetRenderTarget(IDirect3DDevice8 *self,
    IDirect3DSurface8 *rt, IDirect3DSurface8 *zs)
{
    typedef HRESULT (WINAPI *srt_t)(IDirect3DDevice8 *, IDirect3DSurface8 *,
        IDirect3DSurface8 *);
    g_rt_is_backbuffer = (!rt || (g_backbuffer && rt == g_backbuffer));
    return ((srt_t)g_orig_setrendertarget)(self, rt, zs);
}

static HRESULT WINAPI hook_SetRenderState(IDirect3DDevice8 *self,
    D3DRENDERSTATETYPE State, DWORD Value)
{
    typedef HRESULT (WINAPI *srs_t)(IDirect3DDevice8 *, D3DRENDERSTATETYPE,
        DWORD);
    if (State == D3DRS_ALPHABLENDENABLE) g_alpha_blend = Value != 0;
    else if (State == D3DRS_SRCBLEND) g_srcblend = Value;
    return ((srs_t)g_orig_setrenderstate)(self, State, Value);
}

static HRESULT WINAPI hook_SetTexture(IDirect3DDevice8 *self, DWORD stage,
    IDirect3DBaseTexture8 *tex)
{
    typedef HRESULT (WINAPI *stex_t)(IDirect3DDevice8 *, DWORD,
        IDirect3DBaseTexture8 *);
    if (stage == 0) {
        RTTex *rt = find_rt_texture(tex);
        g_stage0_is_rttex = rt != NULL;
        g_stage0_rt_w = rt ? rt->w : 0;
        g_stage0_rt_h = rt ? rt->h : 0;
        g_stage0_rt_fmt = rt ? rt->fmt : 0;
    }
    return ((stex_t)g_orig_settexture)(self, stage, tex);
}

static int postfx_alpha_active(void)
{
    return g_postfx_alpha_fix && g_rt_is_backbuffer && g_stage0_is_rttex &&
           g_alpha_blend && g_srcblend == D3DBLEND_SRCALPHA;
}

static HRESULT postfx_alpha_draw(IDirect3DDevice8 *self, void *ret,
    HRESULT (WINAPI *drawfn)(void *ctx), void *ctx)
{
    typedef HRESULT (WINAPI *srs_t)(IDirect3DDevice8 *, D3DRENDERSTATETYPE,
        DWORD);
    if (!postfx_alpha_active())
        return drawfn(ctx);

    srs_t setrs = (srs_t)g_orig_setrenderstate;
    if (g_postfx_alpha_hits < 8) {
        uint8_t *base = (uint8_t *)GetModuleHandleA(NULL);
        logf_("PostFilterAlphaFix: SRCALPHA -> ONE for RT-texture composite "
              "at exe+0x%tx", base ? (uint8_t *)ret - base : 0);
    }
    g_postfx_alpha_hits++;
    setrs(self, D3DRS_SRCBLEND, D3DBLEND_ONE);
    HRESULT hr = drawfn(ctx);
    setrs(self, D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    return hr;
}

static void drawsite_hit(void *ret, int is_up)
{
    if (!g_drawstats) return;
    uint8_t *base = (uint8_t *)GetModuleHandleA(NULL);
    if (!base) return;
    uint32_t rva = (uint32_t)((uint8_t *)ret - base) & ~0xFu;
    g_draw_site_total++;
    for (int i = 0; i < g_n_draw_sites; i++) {
        if (g_draw_sites[i].rva == rva) {
            g_draw_sites[i].count++;
            if (is_up) g_draw_sites[i].up++;
            return;
        }
    }
    if (g_n_draw_sites < MAX_DRAW_SITES) {
        g_draw_sites[g_n_draw_sites].rva = rva;
        g_draw_sites[g_n_draw_sites].count = 1;
        g_draw_sites[g_n_draw_sites].up = is_up ? 1 : 0;
        g_n_draw_sites++;
    }
}

static void drawsite_top(char *buf, size_t n)
{
    buf[0] = 0;
    size_t off = 0;
    int used[MAX_DRAW_SITES] = {0};
    for (int k = 0; k < 8; k++) {
        int best = -1;
        for (int i = 0; i < g_n_draw_sites; i++)
            if (!used[i] &&
                (best < 0 || g_draw_sites[i].count > g_draw_sites[best].count))
                best = i;
        if (best < 0 || !g_draw_sites[best].count) break;
        used[best] = 1;
        int pct = g_draw_site_total ?
            (int)((g_draw_sites[best].count * 100 + g_draw_site_total / 2) /
                  g_draw_site_total) : 0;
        int w = snprintf(buf + off, n - off, "%sexe+0x%x=%u/%u%s",
                         off ? " " : "", g_draw_sites[best].rva,
                         g_draw_sites[best].count, g_draw_sites[best].up,
                         pct >= 2 ? "" : "*");
        if (w < 0 || (size_t)w >= n - off) break;
        off += w;
    }
}

/* ---- draw/lock diagnostic hooks (only patched in when g_drawstats) ---- */

/* IDirect3DVertexBuffer8::Lock — time the forwarded call and tally DISCARDs.
 * On the D3D->Metal stack a dynamic-VB DISCARD reallocates/flushes per lock
 * (~65us measured previously), so this is where the "A" (lock-churn) cost
 * lands. All VBs share one COM vtable, so slot 11 is patched once. */
static HRESULT WINAPI hook_VBLock(IDirect3DVertexBuffer8 *self, UINT off,
    UINT size, BYTE **ppb, DWORD flags)
{
    typedef HRESULT (WINAPI *lk_t)(IDirect3DVertexBuffer8 *, UINT, UINT,
        BYTE **, DWORD);
    LARGE_INTEGER a, b;
    QueryPerformanceCounter(&a);
    HRESULT hr = ((lk_t)g_orig_vblock)(self, off, size, ppb, flags);
    QueryPerformanceCounter(&b);
    g_f_vblocks++;
    if (flags & D3DLOCK_DISCARD) g_f_discards++;
    g_f_lock_ticks += b.QuadPart - a.QuadPart;
    return hr;
}

static HRESULT WINAPI hook_CreateVertexBuffer(IDirect3DDevice8 *self,
    UINT Length, DWORD Usage, DWORD FVF, D3DPOOL Pool,
    IDirect3DVertexBuffer8 **pp)
{
    typedef HRESULT (WINAPI *cv_t)(IDirect3DDevice8 *, UINT, DWORD, DWORD,
        D3DPOOL, IDirect3DVertexBuffer8 **);
    HRESULT hr = ((cv_t)g_orig_createvb)(self, Length, Usage, FVF, Pool, pp);
    if (SUCCEEDED(hr) && pp && *pp && !g_vb_lock_patched) {
        patch_slot(*pp, IDX_VB_LOCK, (void *)hook_VBLock, &g_orig_vblock);
        g_vb_lock_patched = 1;
        logf_("DRAWSTATS: VB Lock wrapped (shared vtable) — lock timing live");
    }
    return hr;
}

/* The four draw entrypoints time the forwarded call (draw-exec: GPU/driver
 * submission cost lands here, not in Present) and split stream-buffer draws
 * (DrawPrimitive/DrawIndexedPrimitive) from user-pointer draws
 * (DrawPrimitiveUP/DrawIndexedPrimitiveUP — what particle systems typically
 * use, which is why they add draws without adding VB DISCARDs). */
static void drawstats_account(LONGLONG ticks, int is_up)
{
    if (!g_drawstats) return;
    g_f_draws++;
    if (is_up) g_f_up++; else g_f_dip++;
    g_f_draw_ticks += ticks;
}

typedef struct {
    IDirect3DDevice8 *self;
    D3DPRIMITIVETYPE t;
    UINT a, b, c, d, e;
    CONST void *p, *q;
    D3DFORMAT fmt;
    UINT stride;
} DrawCtx;

static HRESULT WINAPI call_DrawPrimitive(void *v)
{
    DrawCtx *c = (DrawCtx *)v;
    typedef HRESULT (WINAPI *dp_t)(IDirect3DDevice8 *, D3DPRIMITIVETYPE, UINT,
        UINT);
    return ((dp_t)g_orig_drawprim)(c->self, c->t, c->a, c->b);
}

static HRESULT WINAPI call_DrawIndexedPrimitive(void *v)
{
    DrawCtx *c = (DrawCtx *)v;
    typedef HRESULT (WINAPI *di_t)(IDirect3DDevice8 *, D3DPRIMITIVETYPE, UINT,
        UINT, UINT, UINT);
    return ((di_t)g_orig_drawindexprim)(c->self, c->t, c->a, c->b, c->c, c->d);
}

static HRESULT WINAPI call_DrawPrimitiveUP(void *v)
{
    DrawCtx *c = (DrawCtx *)v;
    typedef HRESULT (WINAPI *du_t)(IDirect3DDevice8 *, D3DPRIMITIVETYPE, UINT,
        CONST void *, UINT);
    return ((du_t)g_orig_drawprimup)(c->self, c->t, c->a, c->p, c->stride);
}

static HRESULT WINAPI call_DrawIndexedPrimitiveUP(void *v)
{
    DrawCtx *c = (DrawCtx *)v;
    typedef HRESULT (WINAPI *diu_t)(IDirect3DDevice8 *, D3DPRIMITIVETYPE, UINT,
        UINT, UINT, CONST void *, D3DFORMAT, CONST void *, UINT);
    return ((diu_t)g_orig_drawindexprimup)(c->self, c->t, c->a, c->b, c->c,
                                           c->p, c->fmt, c->q, c->stride);
}

static HRESULT timed_draw(IDirect3DDevice8 *self, void *ret,
    HRESULT (WINAPI *fn)(void *), DrawCtx *ctx, int is_up)
{
    LARGE_INTEGER a, b;
    if (g_drawstats) {
        drawsite_hit(ret, is_up);
        QueryPerformanceCounter(&a);
    }
    if (g_postfx_alpha_fix && g_stage0_is_rttex && g_postfx_probe_logs < 24) {
        uint8_t *base = (uint8_t *)GetModuleHandleA(NULL);
        logf_("PostFilterAlphaFix probe: RT texture draw at exe+0x%tx "
              "tex=%ux%u fmt=%d target=%s alpha=%d srcblend=%lu",
              base ? (uint8_t *)ret - base : 0,
              g_stage0_rt_w, g_stage0_rt_h, g_stage0_rt_fmt,
              g_rt_is_backbuffer ? "backbuffer" : "rt",
              g_alpha_blend, (unsigned long)g_srcblend);
        g_postfx_probe_logs++;
    }
    HRESULT hr = postfx_alpha_draw(self, ret, fn, ctx);
    if (g_drawstats) {
        QueryPerformanceCounter(&b);
        drawstats_account(b.QuadPart - a.QuadPart, is_up);
    }
    return hr;
}

static HRESULT WINAPI hook_DrawPrimitive(IDirect3DDevice8 *self,
    D3DPRIMITIVETYPE t, UINT start, UINT count)
{
    DrawCtx c = {0};
    c.self = self; c.t = t; c.a = start; c.b = count;
    return timed_draw(self, __builtin_return_address(0), call_DrawPrimitive,
                      &c, 0);
}

static HRESULT WINAPI hook_DrawIndexedPrimitive(IDirect3DDevice8 *self,
    D3DPRIMITIVETYPE t, UINT minIdx, UINT numV, UINT startIdx, UINT count)
{
    DrawCtx c = {0};
    c.self = self; c.t = t; c.a = minIdx; c.b = numV;
    c.c = startIdx; c.d = count;
    return timed_draw(self, __builtin_return_address(0),
                      call_DrawIndexedPrimitive, &c, 0);
}

static HRESULT WINAPI hook_DrawPrimitiveUP(IDirect3DDevice8 *self,
    D3DPRIMITIVETYPE t, UINT count, CONST void *data, UINT stride)
{
    DrawCtx c = {0};
    c.self = self; c.t = t; c.a = count; c.p = data; c.stride = stride;
    return timed_draw(self, __builtin_return_address(0), call_DrawPrimitiveUP,
                      &c, 1);
}

static HRESULT WINAPI hook_DrawIndexedPrimitiveUP(IDirect3DDevice8 *self,
    D3DPRIMITIVETYPE t, UINT minV, UINT numV, UINT count, CONST void *idx,
    D3DFORMAT idxFmt, CONST void *data, UINT stride)
{
    DrawCtx c = {0};
    c.self = self; c.t = t; c.a = minV; c.b = numV; c.c = count;
    c.p = idx; c.q = data; c.fmt = idxFmt; c.stride = stride;
    return timed_draw(self, __builtin_return_address(0),
                      call_DrawIndexedPrimitiveUP, &c, 1);
}

/* Install the rain-system counter by repointing the single call site's rel32
 * to a stub that does `inc [g_rain_sys_frame]; push helper; ret`. The stub
 * clobbers no registers (only flags, irrelevant across a call boundary) and
 * leaves the stack exactly as a direct `call 0x21d750` would, so 0x21d750 runs
 * unchanged. Verifies the bytes first, so a different build is left alone. */
static void arm_rain_probe(void)
{
    unsigned char *base = (unsigned char *)GetModuleHandleA(NULL);
    if (!base) return;
    unsigned char *site = base + RAIN_CALLSITE_RVA;
    if (site[0] != 0xE8) {
        logf_("rainprobe: exe+0x%x is 0x%02x not a call — not arming "
              "(build mismatch)", RAIN_CALLSITE_RVA, site[0]);
        return;
    }
    unsigned char *target = site + 5 + *(int *)(site + 1);
    if (target != base + RAIN_HELPER_RVA) {
        logf_("rainprobe: call target exe+0x%tx != 0x%x — not arming",
              target - base, RAIN_HELPER_RVA);
        return;
    }
    unsigned char *cave = (unsigned char *)VirtualAlloc(NULL, 16,
        MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!cave) return;
    cave[0] = 0xFF; cave[1] = 0x05;                       /* inc dword [imm32] */
    *(unsigned int *)(cave + 2) = (unsigned int)(uintptr_t)&g_rain_sys_frame;
    cave[6] = 0x68;                                        /* push imm32       */
    *(unsigned int *)(cave + 7) = (unsigned int)(uintptr_t)target;
    cave[11] = 0xC3;                                       /* ret              */
    g_rain_cave = cave;
    DWORD old;
    if (VirtualProtect(site + 1, 4, PAGE_EXECUTE_READWRITE, &old)) {
        *(int *)(site + 1) = (int)(cave - (site + 5));
        VirtualProtect(site + 1, 4, old, &old);
        FlushInstructionCache(GetCurrentProcess(), site, 5);
        logf_("rainprobe: armed at exe+0x%x (cave %p) — per-frame rain-system "
              "count live", RAIN_CALLSITE_RVA, cave);
    }
}

static void arm_rain_emit_cap(void)
{
    unsigned char *base = (unsigned char *)GetModuleHandleA(NULL);
    if (!base || g_rain_emit_cap <= 0) return;

    unsigned char *site = base + RAIN_EMITCAP_RVA;
    if (site[0] != 0x8B || site[1] != 0x44 || site[2] != 0x24 ||
        site[3] != 0x40 || site[4] != 0xDD || site[5] != 0xD8) {
        logf_("RainEmitCap: exe+0x%x bytes are %02x %02x %02x %02x %02x %02x, "
              "not expected — not patching (build mismatch)",
              RAIN_EMITCAP_RVA, site[0], site[1], site[2], site[3], site[4],
              site[5]);
        return;
    }

    unsigned char *cave = (unsigned char *)VirtualAlloc(NULL, 48,
        MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!cave) {
        logf_("RainEmitCap: VirtualAlloc failed");
        return;
    }

    unsigned int cap = (unsigned int)g_rain_emit_cap;
    int p = 0;
    cave[p++] = 0x8B; cave[p++] = 0x44; cave[p++] = 0x24; cave[p++] = 0x40;
    cave[p++] = 0x3D; *(unsigned int *)(cave + p) = cap; p += 4;
    cave[p++] = 0x76; cave[p++] = 0x13;                 /* jbe keep_count */
    cave[p++] = 0xFF; cave[p++] = 0x05;                 /* inc [hits] */
    *(unsigned int *)(cave + p) =
        (unsigned int)(uintptr_t)&g_rain_cap_frame; p += 4;
    cave[p++] = 0xC7; cave[p++] = 0x44; cave[p++] = 0x24; cave[p++] = 0x40;
    *(unsigned int *)(cave + p) = cap; p += 4;           /* [esp+40]=cap */
    cave[p++] = 0xB8; *(unsigned int *)(cave + p) = cap; p += 4;
    cave[p++] = 0xDD; cave[p++] = 0xD8;                 /* fstp st(0) */
    cave[p++] = 0xE9;
    *(int *)(cave + p) = (int)((base + RAIN_EMITCAP_RVA + 6) -
                               (cave + p + 4));
    p += 4;

    DWORD old;
    if (VirtualProtect(site, 6, PAGE_EXECUTE_READWRITE, &old)) {
        site[0] = 0xE9;
        *(int *)(site + 1) = (int)(cave - (site + 5));
        site[5] = 0x90;
        VirtualProtect(site, 6, old, &old);
        FlushInstructionCache(GetCurrentProcess(), site, 6);
        g_rain_cap_cave = cave;
        logf_("RainEmitCap: armed at exe+0x%x, cap=%d quads/system-pass "
              "(cave %p)", RAIN_EMITCAP_RVA, g_rain_emit_cap, cave);
    }
}

static void arm_rain_system_cap(void)
{
    unsigned char *base = (unsigned char *)GetModuleHandleA(NULL);
    if (!base || g_rain_system_cap <= 0) return;

    unsigned char *site = base + RAIN_VISIBLE_GATE_RVA;
    if (site[0] != 0x0F || site[1] != 0x85 ||
        *(int *)(site + 2) !=
            (int)((base + RAIN_VISIBLE_SKIP_RVA) - (site + 6))) {
        logf_("RainSystemCap: exe+0x%x bytes are %02x %02x %02x %02x %02x %02x, "
              "not expected — not patching (build mismatch)",
              RAIN_VISIBLE_GATE_RVA, site[0], site[1], site[2], site[3],
              site[4], site[5]);
        return;
    }

    unsigned char *cave = (unsigned char *)VirtualAlloc(NULL, 64,
        MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!cave) {
        logf_("RainSystemCap: VirtualAlloc failed");
        return;
    }

    unsigned int cap = (unsigned int)g_rain_system_cap;
    unsigned char *skip = base + RAIN_VISIBLE_SKIP_RVA;
    unsigned char *cont = base + RAIN_VISIBLE_CONT_RVA;
    int p = 0;
    cave[p++] = 0x0F; cave[p++] = 0x85;                 /* jne original skip */
    *(int *)(cave + p) = (int)(skip - (cave + p + 4)); p += 4;
    cave[p++] = 0xFF; cave[p++] = 0x05;                 /* inc [visible] */
    *(unsigned int *)(cave + p) =
        (unsigned int)(uintptr_t)&g_rain_visible_frame; p += 4;
    cave[p++] = 0xA1;                                   /* mov eax,[visible] */
    *(unsigned int *)(cave + p) =
        (unsigned int)(uintptr_t)&g_rain_visible_frame; p += 4;
    cave[p++] = 0x3D; *(unsigned int *)(cave + p) = cap; p += 4;
    cave[p++] = 0x76; cave[p++] = 0x0B;                 /* jbe continue */
    cave[p++] = 0xFF; cave[p++] = 0x05;                 /* inc [skipped] */
    *(unsigned int *)(cave + p) =
        (unsigned int)(uintptr_t)&g_rain_sysskip_frame; p += 4;
    cave[p++] = 0xE9;
    *(int *)(cave + p) = (int)(skip - (cave + p + 4)); p += 4;
    cave[p++] = 0xE9;
    *(int *)(cave + p) = (int)(cont - (cave + p + 4)); p += 4;

    DWORD old;
    if (VirtualProtect(site, 6, PAGE_EXECUTE_READWRITE, &old)) {
        site[0] = 0xE9;
        *(int *)(site + 1) = (int)(cave - (site + 5));
        site[5] = 0x90;
        VirtualProtect(site, 6, old, &old);
        FlushInstructionCache(GetCurrentProcess(), site, 6);
        g_rain_system_cave = cave;
        logf_("RainSystemCap: armed at exe+0x%x, cap=%d visible systems/frame "
              "(cave %p)", RAIN_VISIBLE_GATE_RVA, g_rain_system_cap, cave);
    }
}

/* Fold the just-finished frame into the window; log every DRAWSTATS_WINDOW
 * frames. Called from hook_Present with the QPC stamps bracketing the real
 * Present (t0..t1); t1 also serves as the present-to-present frame boundary. */
static void drawstats_frame(LARGE_INTEGER t0, LARGE_INTEGER t1)
{
    g_win_present_ticks += t1.QuadPart - t0.QuadPart;
    if (g_last_frame_end.QuadPart) {
        double ms = (double)(t1.QuadPart - g_last_frame_end.QuadPart) * 1000.0
                    / (double)g_qpf.QuadPart;
        g_win_ms_sum += ms;
        if (ms > g_win_ms_max) g_win_ms_max = ms;
        g_win_frames++;
    }
    g_last_frame_end = t1;

    g_win_draws    += g_f_draws;
    g_win_dip      += g_f_dip;
    g_win_up       += g_f_up;
    g_win_vblocks  += g_f_vblocks;
    g_win_discards += g_f_discards;
    g_win_lock_ticks += g_f_lock_ticks;
    g_win_draw_ticks += g_f_draw_ticks;
    g_win_rainsys += g_rain_sys_frame;
    g_win_raincaps += g_rain_cap_frame;
    g_win_rainvisible += g_rain_visible_frame;
    g_win_rainsysskip += g_rain_sysskip_frame;
    g_f_draws = g_f_vblocks = g_f_discards = g_f_dip = g_f_up = 0;
    g_f_lock_ticks = g_f_draw_ticks = 0;
    g_rain_sys_frame = 0;
    g_rain_cap_frame = 0;
    g_rain_visible_frame = 0;
    g_rain_sysskip_frame = 0;

    if (g_win_frames >= DRAWSTATS_WINDOW) {
        double avg_ms = g_win_ms_sum / g_win_frames;
        double q = (double)g_qpf.QuadPart;
        int n = g_win_frames;
        char drawtops[256];
        drawsite_top(drawtops, sizeof(drawtops));
        logf_("DRAWSTATS %.0ffps avg %.2fms (worst %.2fms) | draws %.0f/f "
              "[strm %.0f up %.0f] exec %.2fms/f | vblk %.0f disc %.0f "
              "lock %.2fms/f | present %.2fms/f | rainsys %.1f/f cap %.1f/f "
              "vis %.1f/f skip %.1f/f",
              avg_ms > 0 ? 1000.0 / avg_ms : 0.0, avg_ms, g_win_ms_max,
              (double)g_win_draws / n, (double)g_win_dip / n,
              (double)g_win_up / n,
              (double)g_win_draw_ticks * 1000.0 / q / n,
              (double)g_win_vblocks / n, (double)g_win_discards / n,
              (double)g_win_lock_ticks * 1000.0 / q / n,
              (double)g_win_present_ticks * 1000.0 / q / n,
              (double)g_win_rainsys / n, (double)g_win_raincaps / n,
              (double)g_win_rainvisible / n,
              (double)g_win_rainsysskip / n);
        if (drawtops[0])
            logf_("  DRAW-hot: %s (format count/up; * = <2%% of draws)",
                  drawtops);
        g_win_frames = 0;
        g_win_ms_sum = g_win_ms_max = 0;
        g_win_draws = g_win_vblocks = g_win_discards = g_win_dip = g_win_up = 0;
        g_win_lock_ticks = g_win_present_ticks = g_win_draw_ticks = 0;
        g_win_rainsys = 0;
        g_win_raincaps = 0;
        g_win_rainvisible = g_win_rainsysskip = 0;
        g_n_draw_sites = 0;
        g_draw_site_total = 0;
    }
}

static HRESULT WINAPI hook_SetViewport(IDirect3DDevice8 *self,
    CONST D3DVIEWPORT8 *vp)
{
    typedef HRESULT (WINAPI *vp_t)(IDirect3DDevice8 *, CONST D3DVIEWPORT8 *);
    /* Hitman Contracts computes a garbage viewport height for resolutions its engine
     * does not recognise (e.g. 1920-wide comes through as H=81528992), which
     * maps the whole scene to a one-pixel sliver. A viewport must fit inside
     * the render target anyway, so any viewport spilling past the backbuffer
     * is clamped to the full backbuffer — that restores correct rendering
     * without touching game code. Legitimate sub-viewports (HUD elements etc.)
     * fit and pass through untouched. */
    D3DVIEWPORT8 fixed;
    if (vp && g_bbw && g_bbh &&
        (vp->X + vp->Width > g_bbw || vp->Y + vp->Height > g_bbh ||
         vp->Width > g_bbw || vp->Height > g_bbh)) {
        if (g_vp_logs < 4) {
            void *ret = __builtin_return_address(0);
            void *exe = (void *)GetModuleHandleA("HitmanContracts.exe");
            logf_("SetViewport caller ret=%p (HitmanContracts.exe=%p)",
                  ret, exe);
        }
        fixed = *vp;
        fixed.X = 0; fixed.Y = 0;
        fixed.Width = g_bbw; fixed.Height = g_bbh;
        if (g_vp_logs < 16) {
            g_vp_logs++;
            logf_("SetViewport %lux%lu at %lu,%lu out of range -> clamped to "
                  "%ux%u", (unsigned long)vp->Width, (unsigned long)vp->Height,
                  (unsigned long)vp->X, (unsigned long)vp->Y, g_bbw, g_bbh);
        }
        return ((vp_t)g_orig_setviewport)(self, &fixed);
    }
    if (vp && g_vp_logs < 16) {
        g_vp_logs++;
        logf_("SetViewport X=%lu Y=%lu W=%lu H=%lu (ok)",
              (unsigned long)vp->X, (unsigned long)vp->Y,
              (unsigned long)vp->Width, (unsigned long)vp->Height);
    }
    return ((vp_t)g_orig_setviewport)(self, vp);
}

static HRESULT WINAPI hook_SetTransform(IDirect3DDevice8 *self,
    D3DTRANSFORMSTATETYPE State, CONST D3DMATRIX *pMatrix)
{
    typedef HRESULT (WINAPI *st_t)(IDirect3DDevice8 *,
        D3DTRANSFORMSTATETYPE, CONST D3DMATRIX *);
    if (State == D3DTS_PROJECTION && pMatrix && g_proj_logs < 24) {
        g_proj_logs++;
        logf_("PROJ in: _11=%.6g _22=%.6g _33=%.6g _34=%.3g _43=%.6g "
              "_44=%.3g  (bb %ux%u)", pMatrix->_11, pMatrix->_22,
              pMatrix->_33, pMatrix->_34, pMatrix->_43, pMatrix->_44,
              g_bbw, g_bbh);
    }
    if (State == D3DTS_PROJECTION && pMatrix) {
        D3DMATRIX m = *pMatrix;
        int any = 0;
        for (int i = 0; i < g_n_hooks; i++)
            if (g_hooks[i].fix_projection) {
                g_hooks[i].fix_projection(&m, g_bbw, g_bbh);
                any = 1;
            }
        if (any)
            return ((st_t)g_orig_settransform)(self, State, &m);
    }
    return ((st_t)g_orig_settransform)(self, State, pMatrix);
}

static HRESULT WINAPI hook_Present(IDirect3DDevice8 *self,
    CONST RECT *src, CONST RECT *dst, HWND override, CONST RGNDATA *dirty)
{
    typedef HRESULT (WINAPI *pr_t)(IDirect3DDevice8 *, CONST RECT *,
        CONST RECT *, HWND, CONST RGNDATA *);
    /* Before the frame is shown: let overlays draw onto the finished back
     * buffer (drawing in on_present, below, would land after Present). */
    for (int i = 0; i < g_n_hooks; i++)
        if (g_hooks[i].on_frame)
            g_hooks[i].on_frame(self);
    LARGE_INTEGER t0;
    if (g_drawstats) QueryPerformanceCounter(&t0);
    HRESULT hr = ((pr_t)g_orig_present)(self, src, dst, override, dirty);
    if (g_drawstats) {
        LARGE_INTEGER t1;
        QueryPerformanceCounter(&t1);
        drawstats_frame(t0, t1);
    }
    /* One displayed frame just finished; let plugins pace the frame rate
     * (the engine's simulation is frame-time bound). */
    for (int i = 0; i < g_n_hooks; i++)
        if (g_hooks[i].on_present)
            g_hooks[i].on_present();
    return hr;
}

static HRESULT WINAPI hook_Reset(IDirect3DDevice8 *self,
    D3DPRESENT_PARAMETERS *pp)
{
    typedef HRESULT (WINAPI *rs_t)(IDirect3DDevice8 *, D3DPRESENT_PARAMETERS *);
    if (pp)
        for (int i = 0; i < g_n_hooks; i++)
            if (g_hooks[i].fix_present)
                g_hooks[i].fix_present(pp, NULL, 1);
    if (pp) { g_bbw = pp->BackBufferWidth; g_bbh = pp->BackBufferHeight; }
    HRESULT hr = ((rs_t)g_orig_reset)(self, pp);
    if (SUCCEEDED(hr) && g_postfx_alpha_fix)
        capture_backbuffer(self);
    return hr;
}

static HRESULT WINAPI hook_CreateDevice(IDirect3D8 *self, UINT Adapter,
    D3DDEVTYPE DeviceType, HWND hFocusWindow, DWORD BehaviorFlags,
    D3DPRESENT_PARAMETERS *pp, IDirect3DDevice8 **ppReturnedDeviceInterface)
{
    typedef HRESULT (WINAPI *cd_t)(IDirect3D8 *, UINT, D3DDEVTYPE, HWND,
        DWORD, D3DPRESENT_PARAMETERS *, IDirect3DDevice8 **);
    if (pp)
        logf_("CreateDevice requested: %ux%u fmt=%d windowed=%d swap=%d",
              pp->BackBufferWidth, pp->BackBufferHeight, pp->BackBufferFormat,
              pp->Windowed, pp->SwapEffect);
    if (pp)
        for (int i = 0; i < g_n_hooks; i++)
            if (g_hooks[i].fix_present)
                g_hooks[i].fix_present(pp, hFocusWindow, 0);
    if (pp)
        logf_("CreateDevice applied:   %ux%u fmt=%d windowed=%d bbcount=%u "
              "interval=0x%lx", pp->BackBufferWidth, pp->BackBufferHeight,
              pp->BackBufferFormat, pp->Windowed, pp->BackBufferCount,
              (unsigned long)pp->FullScreen_PresentationInterval);

    HRESULT hr = ((cd_t)g_orig_createdevice)(self, Adapter, DeviceType,
        hFocusWindow, BehaviorFlags, pp, ppReturnedDeviceInterface);
    logf_("CreateDevice returned 0x%08lx", (unsigned long)hr);

    /* Reliability: an exclusive-fullscreen CreateDevice can fail transiently
     * with D3DERR_NOTAVAILABLE / D3DERR_DEVICELOST during a display-state race
     * (VRR/G-Sync renegotiation, an HDR toggle, the Steam overlay or another
     * app briefly owning the display) — the "this render mode is not available"
     * crash that comes and goes. Retry the same request a few times with a
     * short delay before giving up, turning a flaky start into a reliable one.
     * Windowed requests always create, so this only guards the fullscreen path. */
    if (pp && !pp->Windowed) {
        for (int tries = 0; FAILED(hr) && tries < 5 &&
             (hr == D3DERR_NOTAVAILABLE || hr == D3DERR_DEVICELOST); tries++) {
            logf_("fullscreen CreateDevice failed 0x%08lx — retry %d after 80ms",
                  (unsigned long)hr, tries + 1);
            Sleep(80);
            hr = ((cd_t)g_orig_createdevice)(self, Adapter, DeviceType,
                hFocusWindow, BehaviorFlags, pp, ppReturnedDeviceInterface);
            logf_("fullscreen CreateDevice retry -> 0x%08lx", (unsigned long)hr);
        }
    }

    /* Last-ditch safety net: if the device still failed to create — e.g. a
     * driver rejected a plugin-chosen present interval (a windowed IMMEDIATE
     * interval, say) with D3DERR_INVALIDCALL — fall back to the most
     * conservative parameters that always create (windowed, default interval)
     * so a bad tweak can never leave the game at its "Unable to create device"
     * box. This changes only presentation flags, not the backbuffer size the
     * engine laid out against. */
    if (pp && FAILED(hr)) {
        logf_("CreateDevice still failed 0x%08lx — retrying with safe windowed "
              "defaults (Windowed=1, INTERVAL_DEFAULT)", (unsigned long)hr);
        pp->Windowed = TRUE;
        pp->FullScreen_RefreshRateInHz = 0;
        pp->FullScreen_PresentationInterval = D3DPRESENT_INTERVAL_DEFAULT;
        if (pp->SwapEffect == D3DSWAPEFFECT_FLIP)
            pp->SwapEffect = D3DSWAPEFFECT_DISCARD;
        hr = ((cd_t)g_orig_createdevice)(self, Adapter, DeviceType,
            hFocusWindow, BehaviorFlags, pp, ppReturnedDeviceInterface);
        logf_("safe-fallback CreateDevice -> 0x%08lx", (unsigned long)hr);
    }

    if (SUCCEEDED(hr) && ppReturnedDeviceInterface &&
        *ppReturnedDeviceInterface) {
        if (pp) { g_bbw = pp->BackBufferWidth; g_bbh = pp->BackBufferHeight; }
        patch_slot(*ppReturnedDeviceInterface, IDX_DEV_RESET,
                   (void *)hook_Reset, &g_orig_reset);
        patch_slot(*ppReturnedDeviceInterface, IDX_DEV_PRESENT,
                   (void *)hook_Present, &g_orig_present);
        patch_slot(*ppReturnedDeviceInterface, IDX_DEV_SETTRANSFORM,
                   (void *)hook_SetTransform, &g_orig_settransform);
        patch_slot(*ppReturnedDeviceInterface, IDX_DEV_SETVIEWPORT,
                   (void *)hook_SetViewport, &g_orig_setviewport);
        patch_slot(*ppReturnedDeviceInterface, IDX_DEV_CREATETEXTURE,
                   (void *)hook_CreateTexture, &g_orig_createtexture);
        patch_slot(*ppReturnedDeviceInterface, IDX_DEV_CREATERENDERTGT,
                   (void *)hook_CreateRenderTarget, &g_orig_createrendertgt);
        patch_slot(*ppReturnedDeviceInterface, IDX_DEV_CREATEIMAGESURF,
                   (void *)hook_CreateImageSurface, &g_orig_createimagesurf);
        if (g_postfx_alpha_fix) {
            patch_slot(*ppReturnedDeviceInterface, IDX_DEV_SETRENDERTARGET,
                       (void *)hook_SetRenderTarget, &g_orig_setrendertarget);
            patch_slot(*ppReturnedDeviceInterface, IDX_DEV_SETRENDERSTATE,
                       (void *)hook_SetRenderState, &g_orig_setrenderstate);
            patch_slot(*ppReturnedDeviceInterface, IDX_DEV_SETTEXTURE,
                       (void *)hook_SetTexture, &g_orig_settexture);
            capture_backbuffer(*ppReturnedDeviceInterface);
            logf_("PostFilterAlphaFix: armed (RT texture -> backbuffer "
                  "SRCALPHA composites use ONE)");
        }
        if (g_drawstats) {
            patch_slot(*ppReturnedDeviceInterface, IDX_DEV_CREATEVB,
                       (void *)hook_CreateVertexBuffer, &g_orig_createvb);
        }
        if (g_drawstats || g_postfx_alpha_fix) {
            patch_slot(*ppReturnedDeviceInterface, IDX_DEV_DRAWPRIM,
                       (void *)hook_DrawPrimitive, &g_orig_drawprim);
            patch_slot(*ppReturnedDeviceInterface, IDX_DEV_DRAWINDEXPRIM,
                       (void *)hook_DrawIndexedPrimitive, &g_orig_drawindexprim);
            patch_slot(*ppReturnedDeviceInterface, IDX_DEV_DRAWPRIMUP,
                       (void *)hook_DrawPrimitiveUP, &g_orig_drawprimup);
            patch_slot(*ppReturnedDeviceInterface, IDX_DEV_DRAWINDEXPRIMUP,
                       (void *)hook_DrawIndexedPrimitiveUP,
                       &g_orig_drawindexprimup);
            if (g_drawstats)
                logf_("DRAWSTATS: draw/lock probe armed (window=%d frames)",
                      DRAWSTATS_WINDOW);
        }
        for (int i = 0; i < g_n_hooks; i++)
            if (g_hooks[i].on_device)
                g_hooks[i].on_device(*ppReturnedDeviceInterface);
        logf_("device %p wrapped (Reset+Present+SetTransform+SetViewport)",
              *ppReturnedDeviceInterface);
    }
    return hr;
}

/* ---- modern resolution list (ModernModes) ----------------------------
 *
 * Contracts builds its video-options resolution list straight from D3D8:
 * GetAdapterModeCount + an EnumAdapterModes loop (exe+0x1e38ae..0x1e396d)
 * filtered to D3DFMT_X8R8G8B8 and deduped by WxH — the menu then formats the
 * surviving entries as "%dx%d". Under CrossOver that enumeration returns
 * winemac's list of *scaled Mac desktop modes* (e.g. 1147x745, 1352x878,
 * 1512x982 on a 14" MacBook), so the in-game toggle offers nothing a game
 * would normally run at, and on a Retina panel the logical (points) sizes
 * understate what the device can actually show — the panel is 2x the
 * reported desktop. On Windows the raw list works but is noisy and mixes in
 * legacy 4:3 modes.
 *
 * With ModernModes=1 (HMCWidescreen.ini, default on) the loader instead
 * serves the game a curated list: the 16:9 and 16:10 resolutions games
 * typically offer, limited to what the device supports — capped at the
 * largest real enumerated display mode, raised under Wine to 2x the logical
 * desktop (the Retina backing scale winemac hides), and always including the
 * current HitmanContracts.ini resolution so the active setting stays
 * selectable. The borderless/letterbox presenter handles any of these sizes
 * on any screen; on real Windows the exclusive-fullscreen path still
 * validates against real modes (is_display_mode) and falls back to
 * borderless for sizes the display cannot mode-switch to, so every offered
 * entry is operational there too. Selecting an entry the engine already
 * knows end-to-end (it lands in its own mode array) also spares us its
 * unknown-resolution snap quirks. */
static void *g_orig_modecount;
static void *g_orig_enummodes;
static int g_modern_modes = 1;      /* ModernModes ini toggle */
static D3DDISPLAYMODE g_curated[24];
static int g_n_curated = -1;        /* -1 not built yet; 0 = passthrough */

static const struct { unsigned short w, h; } k_modern[] = {
    {1280, 720}, {1280, 800}, {1366, 768}, {1440, 900}, {1600, 900},
    {1680, 1050}, {1920, 1080}, {1920, 1200}, {2560, 1440}, {2560, 1600},
    {3840, 2160},
};

static int is_wine_host(void)
{
    return GetProcAddress(GetModuleHandleA("ntdll.dll"),
                          "wine_get_version") != NULL;
}

/* Parse "Resolution WxH" from HitmanContracts.ini in the game root (this
 * module's directory), mirroring the widescreen plugin's reader. */
static void read_game_ini_resolution(int *w, int *h)
{
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s\\HitmanContracts.ini", g_dir);
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (_strnicmp(p, "Resolution", 10) != 0) continue;
        int rw = 0, rh = 0;
        if (sscanf(p + 10, " %dx%d", &rw, &rh) == 2 &&
            rw >= 320 && rh >= 200 && rw <= 16384 && rh <= 16384) {
            *w = rw; *h = rh;
        }
        break;
    }
    fclose(f);
}

static void build_curated_modes(void)
{
    if (g_n_curated >= 0) return;
    g_n_curated = 0;                /* passthrough unless built below */
    if (!g_modern_modes) return;

    /* Device capability cap. The largest real enumerated mode is the native
     * resolution on Windows. Under Wine/winemac the enumeration is in Mac
     * "points" and understates a Retina panel by the 2x backing scale, so
     * raise the cap to 2x the logical desktop as well (a 1512x982 14" MacBook
     * really drives 3024x1964 pixels). */
    int dw = GetSystemMetrics(SM_CXSCREEN);
    int dh = GetSystemMetrics(SM_CYSCREEN);
    int capw = 0, caph = 0;
    DEVMODEA dm;
    memset(&dm, 0, sizeof(dm));
    dm.dmSize = sizeof(dm);
    for (DWORD i = 0; EnumDisplaySettingsA(NULL, i, &dm); i++) {
        if (dm.dmBitsPerPel < 32) continue;
        if ((int)dm.dmPelsWidth > capw) capw = (int)dm.dmPelsWidth;
        if ((int)dm.dmPelsHeight > caph) caph = (int)dm.dmPelsHeight;
    }
    if (is_wine_host()) {
        if (2 * dw > capw) capw = 2 * dw;
        if (2 * dh > caph) caph = 2 * dh;
    }
    if (capw <= 0 || caph <= 0) { capw = dw; caph = dh; }
    if (capw <= 0 || caph <= 0) {
        logf_("ModernModes: no usable display metrics — passing the real "
              "mode list through");
        return;
    }

    int n = 0;
    for (unsigned i = 0; i < sizeof(k_modern) / sizeof(k_modern[0]); i++) {
        if (k_modern[i].w > capw || k_modern[i].h > caph) continue;
        g_curated[n].Width = k_modern[i].w;
        g_curated[n].Height = k_modern[i].h;
        g_curated[n].RefreshRate = 0;          /* engine ignores it */
        g_curated[n].Format = D3DFMT_X8R8G8B8; /* the engine's filter */
        n++;
    }
    /* Keep the active setting selectable even if it is non-standard. */
    int iw = 0, ih = 0;
    read_game_ini_resolution(&iw, &ih);
    if (iw && ih && iw <= capw && ih <= caph &&
        n < (int)(sizeof(g_curated) / sizeof(g_curated[0]))) {
        int dup = 0;
        for (int i = 0; i < n; i++)
            if ((int)g_curated[i].Width == iw &&
                (int)g_curated[i].Height == ih) { dup = 1; break; }
        if (!dup) {
            g_curated[n].Width = (UINT)iw;
            g_curated[n].Height = (UINT)ih;
            g_curated[n].RefreshRate = 0;
            g_curated[n].Format = D3DFMT_X8R8G8B8;
            n++;
        }
    }
    if (n == 0) {
        logf_("ModernModes: no candidate fits the %dx%d cap — passing the "
              "real mode list through", capw, caph);
        return;
    }

    /* Sentinel: duplicate the last mode with a different refresh rate. The
     * game's list builder (exe+0x1e38ae) allocates GetAdapterModeCount()*16
     * bytes, stores one 16-byte entry per SURVIVING mode (X8R8G8B8 filter +
     * adjacent same-WxH dedup), then writes a 16-byte zero TERMINATOR at the
     * survivor count (exe+0x1e398f). Real enumerations always lose entries
     * to that filtering (refresh-rate duplicates, 16-bit modes), so the
     * terminator lands inside the allocation — but a fully-unique all-X8
     * curated list made survivors == count, and the terminator overflowed
     * the block by 16 bytes, corrupting the engine's pool allocator (int3
     * assert in its block finder at exe+0x216927 shortly after startup).
     * One adjacent duplicate gets deduped, keeping survivors == count-1 and
     * the terminator in bounds, and is indistinguishable from what real
     * drivers return anyway. */
    g_curated[n] = g_curated[n - 1];
    g_curated[n].RefreshRate = 60;
    n++;
    g_n_curated = n;

    char list[512];
    size_t off = 0;
    for (int i = 0; i < n && off < sizeof(list) - 16; i++)
        off += (size_t)snprintf(list + off, sizeof(list) - off, "%s%ux%u",
                                i ? " " : "", g_curated[i].Width,
                                g_curated[i].Height);
    logf_("ModernModes: serving %d modes (cap %dx%d, desktop %dx%d%s): %s",
          n, capw, caph, dw, dh, is_wine_host() ? ", wine 2x rule" : "", list);
}

static UINT WINAPI hook_GetAdapterModeCount(IDirect3D8 *self, UINT adapter)
{
    typedef UINT (WINAPI *mc_t)(IDirect3D8 *, UINT);
    build_curated_modes();
    if (g_n_curated > 0)
        return (UINT)g_n_curated;
    return ((mc_t)g_orig_modecount)(self, adapter);
}

static HRESULT WINAPI hook_EnumAdapterModes(IDirect3D8 *self, UINT adapter,
    UINT idx, D3DDISPLAYMODE *mode)
{
    typedef HRESULT (WINAPI *em_t)(IDirect3D8 *, UINT, UINT,
                                   D3DDISPLAYMODE *);
    build_curated_modes();
    if (g_n_curated > 0) {
        if (!mode || idx >= (UINT)g_n_curated)
            return D3DERR_INVALIDCALL;
        *mode = g_curated[idx];
        return D3D_OK;
    }
    return ((em_t)g_orig_enummodes)(self, adapter, idx, mode);
}

/* ---- exports --------------------------------------------------------- */

__declspec(dllexport) IDirect3D8 * WINAPI Direct3DCreate8(UINT SDKVersion)
{
    typedef IDirect3D8 * (WINAPI *dc_t)(UINT);
    dc_t real = (dc_t)(uintptr_t)resolve("Direct3DCreate8");
    if (!real) return NULL;
    IDirect3D8 *d3d = real(SDKVersion);
    if (d3d) {
        patch_slot(d3d, IDX_D3D8_CREATEDEVICE, (void *)hook_CreateDevice,
                   &g_orig_createdevice);
        patch_slot(d3d, IDX_D3D8_GETMODECOUNT,
                   (void *)hook_GetAdapterModeCount, &g_orig_modecount);
        patch_slot(d3d, IDX_D3D8_ENUMMODES, (void *)hook_EnumAdapterModes,
                   &g_orig_enummodes);
        logf_("Direct3DCreate8(%u) -> %p; CreateDevice + mode enumeration "
              "wrapped", SDKVersion, d3d);
    } else {
        logf_("real Direct3DCreate8 returned NULL");
    }
    return d3d;
}

/* Is w x h one of the modes ModernModes offered the game's resolution menu?
 * Used by the widescreen plugin to recognise (and honour) an in-game
 * resolution switch at device Reset. Returns 0 when ModernModes is off or
 * passing through, so callers keep their legacy behaviour. */
__declspec(dllexport) int WINAPI HMC_IsCuratedMode(unsigned int w,
                                                   unsigned int h)
{
    build_curated_modes();
    for (int i = 0; i < g_n_curated; i++)
        if (g_curated[i].Width == w && g_curated[i].Height == h)
            return 1;
    return 0;
}

/* The remaining four exports are forwarded verbatim; the game never calls
 * them, but a correct drop-in must provide them at the right ordinals. */
__declspec(dllexport) HRESULT WINAPI ValidatePixelShader(
    DWORD *a, DWORD *b, BOOL c, char **d)
{
    HRESULT (WINAPI *f)(DWORD *, DWORD *, BOOL, char **) =
        (void *)(uintptr_t)resolve("ValidatePixelShader");
    return f ? f(a, b, c, d) : E_FAIL;
}

__declspec(dllexport) HRESULT WINAPI ValidateVertexShader(
    DWORD *a, DWORD *b, DWORD *c, BOOL d, char **e)
{
    HRESULT (WINAPI *f)(DWORD *, DWORD *, DWORD *, BOOL, char **) =
        (void *)(uintptr_t)resolve("ValidateVertexShader");
    return f ? f(a, b, c, d, e) : E_FAIL;
}

__declspec(dllexport) void WINAPI DebugSetMute(void)
{
    void (WINAPI *f)(void) = (void *)(uintptr_t)resolve("DebugSetMute");
    if (f) f();
}

__declspec(dllexport) DWORD WINAPI D3D8GetSWInfo(void)
{
    DWORD (WINAPI *f)(void) = (void *)(uintptr_t)resolve("D3D8GetSWInfo");
    return f ? f() : 0;
}

/* Registration entry point used by the ASI plugins. */
__declspec(dllexport) void WINAPI HMC_RegisterD3D8Hooks(
    const HMCD3D8Hooks *hooks)
{
    if (!hooks || hooks->version != HMC_D3D8_HOOKS_VERSION) {
        logf_("HMC_RegisterD3D8Hooks: version mismatch (%u != %u)",
              hooks ? hooks->version : 0, HMC_D3D8_HOOKS_VERSION);
        return;
    }
    if (g_n_hooks >= MAX_HOOKSETS) {
        logf_("HMC_RegisterD3D8Hooks: too many plugins (max %d)", MAX_HOOKSETS);
        return;
    }
    HMCD3D8Hooks *h = &g_hooks[g_n_hooks++];
    *h = *hooks;
    logf_("plugin #%d registered D3D8 hooks (fix_present=%p fix_projection=%p "
          "on_device=%p on_frame=%p on_present=%p)", g_n_hooks,
          (void *)h->fix_present, (void *)h->fix_projection,
          (void *)h->on_device, (void *)h->on_frame, (void *)h->on_present);
}

/* ---- ASI loading ----------------------------------------------------- */

static void load_asis(void)
{
    char pat[MAX_PATH];
    snprintf(pat, sizeof(pat), "%s\\scripts\\*.asi", g_dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        logf_("no ASI plugins found (%s)", pat);
        return;
    }
    int n = 0, fail = 0;
    do {
        char path[MAX_PATH];
        snprintf(path, sizeof(path), "%s\\scripts\\%s", g_dir, fd.cFileName);
        HMODULE m = LoadLibraryA(path);
        if (m) {
            n++;
            logf_("loaded %s at %p", fd.cFileName, m);
        } else {
            fail++;
            logf_("FAILED to load %s (error %lu)", fd.cFileName,
                  (unsigned long)GetLastError());
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    logf_("%d plugin(s) loaded%s", n, fail ? ", some failed!" : "");
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = inst;
        DisableThreadLibraryCalls(inst);
        GetModuleFileNameA(inst, g_dir, sizeof(g_dir));
        char *sl = strrchr(g_dir, '\\');
        if (sl) *sl = 0;
        char logpath[MAX_PATH];
        snprintf(logpath, sizeof(logpath), "%s\\scripts\\HMCAsiLoader.log",
                 g_dir);
        g_log = fopen(logpath, "w");
        logf_("HMC ASI Loader (d3d8.dll proxy) attached");
        /* Enable the draw/lock probe via either HMC_DRAWSTATS=1 (env) or a
         * marker file scripts/DRAWSTATS next to the plugins — the file is the
         * easy toggle when the game is launched through Steam in a CrossOver
         * bottle, where injecting an env var is awkward. */
        char v[8] = {0};
        char marker[MAX_PATH];
        snprintf(marker, sizeof(marker), "%s\\scripts\\DRAWSTATS", g_dir);
        int env_on = GetEnvironmentVariableA("HMC_DRAWSTATS", v, sizeof(v)) &&
                     v[0] == '1';
        int file_on = GetFileAttributesA(marker) != INVALID_FILE_ATTRIBUTES;
        if (env_on || file_on) {
            g_drawstats = 1;
            QueryPerformanceFrequency(&g_qpf);
            logf_("draw/lock/present probe ENABLED (%s; logs every %d frames)",
                  file_on ? "scripts/DRAWSTATS marker" : "HMC_DRAWSTATS=1",
                  DRAWSTATS_WINDOW);
            arm_rain_probe();
        }
        char pfx_marker[MAX_PATH];
        snprintf(pfx_marker, sizeof(pfx_marker),
                 "%s\\scripts\\POSTFX_ALPHA_FIX", g_dir);
        memset(v, 0, sizeof(v));
        int pfx_env = GetEnvironmentVariableA("HMC_POSTFX_ALPHA_FIX", v,
                                              sizeof(v)) && v[0] == '1';
        int pfx_file = GetFileAttributesA(pfx_marker) != INVALID_FILE_ATTRIBUTES;
        int pfx_ini = read_postfx_alpha_ini();
        if (pfx_env || pfx_file || pfx_ini) {
            g_postfx_alpha_fix = 1;
            logf_("PostFilterAlphaFix ENABLED (%s)",
                  pfx_ini ? "HMCWidescreen.ini" :
                  (pfx_file ? "scripts/POSTFX_ALPHA_FIX marker" :
                   "HMC_POSTFX_ALPHA_FIX=1"));
        }
        /* Modern in-game resolution list. Follows the widescreen plugin's
         * master Enabled switch: with the plugin off the game runs its stock
         * path and gets the real enumeration. */
        g_modern_modes = read_int_ini("Enabled", 1) &&
                         read_int_ini("ModernModes", 1);
        logf_("ModernModes %s (curated 16:9/16:10 in-game resolution list)",
              g_modern_modes ? "ENABLED" : "disabled");
        memset(v, 0, sizeof(v));
        int pfox_env = GetEnvironmentVariableA("HMC_POSTFX_OPAQUE_RT", v,
                                               sizeof(v)) && v[0] == '1';
        int pfox_ini = read_int_ini("PostFilterOpaqueRT", 0);
        if (pfox_env || pfox_ini) {
            g_postfx_opaque_rt = 1;
            /* Optional subset mask. Default 0x3f = all six sites (legacy). Set a
             * value with the death-grade buffer's bit clear to keep bloom X8
             * while the dying/slow-mo B&W desaturation keeps its A8 alpha. */
            int mask_ini = read_int_ini("PostFilterOpaqueRTMask", -1);
            if (mask_ini >= 0)
                g_postfx_opaque_mask = (unsigned)mask_ini & 0x3f;
            logf_("PostFilterOpaqueRT ENABLED (%s), site mask 0x%02x",
                  pfox_ini ? "HMCWidescreen.ini" : "HMC_POSTFX_OPAQUE_RT=1",
                  g_postfx_opaque_mask);
        }
        memset(v, 0, sizeof(v));
        int rain_env = GetEnvironmentVariableA("HMC_RAIN_EMIT_CAP", v,
                                               sizeof(v)) ? atoi(v) : 0;
        int rain_ini = read_int_ini("RainEmitCap", 0);
        g_rain_emit_cap = rain_env > 0 ? rain_env : rain_ini;
        if (g_rain_emit_cap > 0) {
            if (g_rain_emit_cap < 16) g_rain_emit_cap = 16;
            if (g_rain_emit_cap > 4096) g_rain_emit_cap = 4096;
            logf_("RainEmitCap ENABLED (%s, cap=%d)",
                  rain_env > 0 ? "HMC_RAIN_EMIT_CAP" : "HMCWidescreen.ini",
                  g_rain_emit_cap);
            arm_rain_emit_cap();
        }
        memset(v, 0, sizeof(v));
        int rsys_env = GetEnvironmentVariableA("HMC_RAIN_SYSTEM_CAP", v,
                                               sizeof(v)) ? atoi(v) : 0;
        int rsys_ini = read_int_ini("RainSystemCap", 0);
        g_rain_system_cap = rsys_env > 0 ? rsys_env : rsys_ini;
        if (g_rain_system_cap > 0) {
            if (g_rain_system_cap < 4) g_rain_system_cap = 4;
            if (g_rain_system_cap > 256) g_rain_system_cap = 256;
            logf_("RainSystemCap ENABLED (%s, cap=%d)",
                  rsys_env > 0 ? "HMC_RAIN_SYSTEM_CAP" : "HMCWidescreen.ini",
                  g_rain_system_cap);
            arm_rain_system_cap();
        }
        load_asis();
    }
    return TRUE;
}
