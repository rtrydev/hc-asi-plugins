# mac/Linux: mingw-w64 cross compiler (brew install mingw-w64).
# Windows: MSYS2's 32-bit native gcc (pacman -S mingw-w64-i686-gcc); run
# make with C:\msys64\mingw32\bin on PATH or via ..\build.ps1.
ifeq ($(OS),Windows_NT)
CC := gcc
else
CC := i686-w64-mingw32-gcc
endif
CFLAGS := -O2 -Wall -Wextra -msse2 -static-libgcc
LDFLAGS := -shared -Wl,--kill-at -static -lkernel32

.PHONY: all diag clean

all: ../dist/d3d8.dll ../dist/hmc_display.asi ../dist/hmc_reduced_x87.asi

# diagnostic x87 build (helper NaN tracing); built on demand, not installed
diag: ../dist/hmc_reduced_x87_diag.asi

../dist/d3d8.dll: asiloader.c d3d8.def hmc_d3d8.h
	@mkdir -p ../dist
	$(CC) $(CFLAGS) -o $@ asiloader.c d3d8.def $(LDFLAGS) \
	      -Wl,--enable-stdcall-fixup -luser32 -lgdi32

../dist/hmc_display.asi: display_main.c widescreen.c profiler.c uiscale.c \
                         hmc_d3d8.h hmc_plugin.h
	@mkdir -p ../dist
	$(CC) $(CFLAGS) -o $@ display_main.c widescreen.c profiler.c uiscale.c \
	      $(LDFLAGS) -luser32 -lgdi32 -lwinmm

../dist/hmc_reduced_x87.asi: hmcx87.c helpers.S
	@mkdir -p ../dist
	$(CC) $(CFLAGS) -o $@ hmcx87.c helpers.S $(LDFLAGS)

../dist/hmc_reduced_x87_diag.asi: hmcx87.c helpers.S
	@mkdir -p ../dist
	$(CC) $(CFLAGS) -DHMC_DIAG -o $@ hmcx87.c helpers.S $(LDFLAGS)

clean:
	rm -f ../dist/d3d8.dll ../dist/hmc_display.asi \
	      ../dist/hmc_reduced_x87.asi ../dist/hmc_reduced_x87_diag.asi
