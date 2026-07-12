/* hmc_display.asi — entry point for the combined display plugin.
 *
 * Bundles the two D3D8-hook features that used to ship as separate ASIs:
 *   - widescreen.c : startup fix (windowed/borderless device), Hor+ FOV,
 *                    frame pacing, cursor/mouse fixes, post-filter patch;
 *   - profiler.c   : top-right FPS/frame-time overlay + EIP-sampled CPU
 *                    breakdown (X87/GAME/REST).
 * Both register their own HMCD3D8Hooks set with the d3d8.dll loader, which
 * invokes every registered hook in turn, so merging them into one module
 * changes nothing about how they attach.
 *
 * Config: scripts/hmc_display.ini — [display] section for widescreen.c,
 * [profiler] section for profiler.c. Log: scripts/hmc_display.log (shared,
 * lines tagged with the feature that wrote them).
 */
#include <windows.h>
#include <stdio.h>
#include "hmc_plugin.h"

static FILE *g_log;
static CRITICAL_SECTION g_log_lock;

void hmc_vlogf(const char *tag, const char *fmt, va_list ap)
{
    if (!g_log) return;
    EnterCriticalSection(&g_log_lock);
    fprintf(g_log, "[%s] ", tag);
    vfprintf(g_log, fmt, ap);
    fputc('\n', g_log);
    fflush(g_log);
    LeaveCriticalSection(&g_log_lock);
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        InitializeCriticalSection(&g_log_lock);
        char dir[MAX_PATH], logpath[MAX_PATH];
        GetModuleFileNameA(inst, dir, sizeof(dir));
        char *sl = strrchr(dir, '\\');
        if (sl) *sl = 0;
        snprintf(logpath, sizeof(logpath), "%s\\hmc_display.log", dir);
        g_log = fopen(logpath, "w");
        hmc_widescreen_init(inst);
        hmc_profiler_init(inst);
    } else if (reason == DLL_PROCESS_DETACH) {
        hmc_widescreen_detach();   /* UIScale: undo an engine-saved layout
                                    * resolution in HitmanContracts.ini */
        hmc_profiler_detach();
    }
    return TRUE;
}
