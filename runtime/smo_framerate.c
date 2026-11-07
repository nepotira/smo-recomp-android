/* smo_framerate.c - frame_rate = 30 (graphics.ini): 30 frames a second, the game at full speed.
 *
 * SMO advances its world one step per frame (sead::GameFrameworkNx::procCalc_ [main 0x731fdc] runs every task's calc
 * once), so below 60 frames a second it plays slowly. With frame_rate = 30 the presents come every second refresh
 * (display.c) and every procCalc_ runs the calc twice: 60 steps a second, as on the console, with half the drawing
 * (procDraw_ still runs once a frame). The calc is reached through the framework's vtable (call_indirect, the
 * function table) and libsmo.so is linked with -Wl,--wrap=f_4000731fdc (tools/android/build_runtime.py), so every
 * call comes here. procCalc_ takes only `this` (x0): the second call gets x0 and the return address back; the
 * registers a callee must keep are kept by the call itself. Android port (2026-11-02). */
#include <stdio.h>
#include <stdlib.h>
#include "plat.h"
#include "runtime.h"
#include "gfx_options.h"

void __real_f_4000731fdc(Ctx *c);
static int g_twice = -1;
static int64_t g_calcs;

void __wrap_f_4000731fdc(Ctx *c) {
    if (g_twice < 0) {
        const char *e = getenv("SMO_CALC_TWICE");                    /* test switch; graphics.ini frame_rate = 30 is the setting */
        g_twice = e ? atoi(e) != 0 : gfx_options()->fps30;
        printf("smo_framerate: %s\n", g_twice ? "frame_rate = 30: the game's logic runs twice a frame (full speed at 30 frames a second)"
                                              : "frame_rate = 60: the game's logic once a frame");
    }
    if (!g_twice) { __real_f_4000731fdc(c); return; }
    uint64_t self = c->x[0], ret = c->x[30];
    __real_f_4000731fdc(c);
    c->x[0] = self; c->x[30] = ret;
    __real_f_4000731fdc(c);
    if (++g_calcs % 1800 == 0) { printf("smo_framerate: %lld frames with two logic steps\n", (long long)g_calcs); fflush(stdout); }
}
