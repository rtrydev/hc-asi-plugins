/* Internal interface between the hmc_display.asi entry point
 * (display_main.c) and the feature translation units it bundles
 * (widescreen.c, profiler.c). Both features are D3D8-hook plugins that
 * register with the d3d8.dll loader's hook chain, so they ship as one ASI:
 * display_main.c owns DllMain and the shared log (scripts/hmc_display.log);
 * each feature keeps its own static logf_ that forwards here with a tag. */
#ifndef HMC_PLUGIN_H
#define HMC_PLUGIN_H

#include <windows.h>
#include <stdarg.h>

/* shared log sink; tag prefixes the line ("widescreen" / "profiler") */
void hmc_vlogf(const char *tag, const char *fmt, va_list ap);

/* feature entry points, called from display_main.c's DllMain */
void hmc_widescreen_init(HINSTANCE inst);
void hmc_widescreen_detach(void);
void hmc_profiler_init(HINSTANCE inst);
void hmc_profiler_detach(void);

/* uiscale.c — believed-resolution UI scaling. widescreen.c forwards the
 * [display] UIScale config, decides the backbuffer size in fix_present and
 * calls setup/off; the fix_viewport hook goes into widescreen.c's v4 hook
 * registration (declared there — it is D3D-typed). The profiler multiplies
 * its glyph size by hmc_uiscale_k(). */
void  hmc_uiscale_config(float uiscale);
void  hmc_uiscale_patchmask(unsigned int mask);
float hmc_uiscale_cfg(void);
int   hmc_uiscale_wanted(void);
int   hmc_uiscale_rebelieve(int rw, int rh, int lw, int lh);
int   hmc_uiscale_reassert(void);
/* Temporal split: assert the REAL render resolution across the engine's 3D
 * pass (layout_phase 0) and the divided layout resolution only for its 2D
 * layer (layout_phase 1). Driven by the loader's v5 set_ui_phase hook. */
void  hmc_uiscale_phase(int layout_phase);
void  hmc_uiscale_phase_split(int on);
int   hmc_uiscale_force_lod0(void);
void  hmc_uiscale_setup(int ini_w, int ini_h,
                        unsigned bb_w, unsigned bb_h);
void  hmc_uiscale_setup_viewport(int ini_w, int ini_h,
                                 unsigned bb_w, unsigned bb_h);
void  hmc_uiscale_off(void);
float hmc_uiscale_k(void);

#endif /* HMC_PLUGIN_H */
