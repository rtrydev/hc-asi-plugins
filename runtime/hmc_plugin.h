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
void hmc_profiler_init(HINSTANCE inst);
void hmc_profiler_detach(void);

#endif /* HMC_PLUGIN_H */
