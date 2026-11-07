/* smo_quality.c - dynamic_resolution = off (graphics.ini): the game's 3D at the full size it asks for.
 *
 * al::GraphicsQualityInfo::calcRenderResolution [main 0xa58394] (out, requested size, view info) gives a view's render
 * size: a fixed 680 x 680 for one kind of view (view info +4 set), otherwise the requested size capped by the quality
 * info's own size, which al::GraphicsQualityController lowers when frames run long. Here frames run long because of the
 * CPU, so the cap only made the 3D coarse (800 x 450 measured on a Galaxy S24 Ultra) without helping the frame rate.
 * With dynamic_resolution = off the cap is not applied. libsmo.so is linked with -Wl,--wrap=f_4000a58394
 * (tools/android/build_runtime.py); the one caller is in another object. Android port (2026-11-02). */
#include <stdio.h>
#include <stdlib.h>
#include "plat.h"
#include "runtime.h"
#include "gfx_options.h"

void __real_f_4000a58394(Ctx *c);
static int g_full = -1;

/* 0.1.8: with real GPU timers (0.1.7) the game's controller moved its cap about 5 times a second on a phone (S24 Ultra, 60 fps
 * mode: 300 changes in a minute), between 800 and 1600 wide, and the game makes its render targets again at each new size:
 * Cascade 44 fps with that, 54-57 without. Here the main view (the largest size asked for) follows the cap slowly and in steps:
 * a cap under the size for RES_DOWN_MS takes it one step down, a cap over it for g_up_ms one step up. When a step up is taken
 * back soon (RES_FAILED_MS) the wait for the next one doubles (up to RES_UP_MAX_MS), so the size settles where the GPU keeps up
 * and tries more only now and then. Views that ask for less go through untouched.
 * SMO_RES_HOLD=0: the game's cap as it is. SMO_RES_FIX=<width>: that width all the time (a test). */
#define RES_DOWN_MS    1000
#define RES_UP_MS      8000
#define RES_UP_MAX_MS  128000
#define RES_FAILED_MS  16000
#define RES_STEP       96
static void res_hold(uint64_t out, uint64_t in) {
    static int on = -1, fix, g_up_ms = RES_UP_MS, want_dir;      /* want_dir: the cap's side of the size, and since when */
    static uint32_t main_w, held_w;
    static int64_t want_at, up_at;
    static long long changes;
    if (on < 0) {
        const char *e = getenv("SMO_RES_HOLD");
        on = !e || atoi(e) != 0;
        fix = (e = getenv("SMO_RES_FIX")) ? atoi(e) : 0;
        printf("smo_quality: render size %s\n", fix ? "fixed (SMO_RES_FIX)" : on ? "follows the game's cap slowly, in steps" : "as the game's cap (SMO_RES_HOLD=0)");
    }
    if (!on) return;
    uint32_t iw = rd32(in), ih = rd32(in + 4), w = rd32(out);
    if (iw > main_w) { main_w = iw; held_w = 0; }  /* a larger main view: start from its cap */
    if (iw != main_w) return;
    if (fix) { w = (uint32_t)fix < iw ? (uint32_t)fix : iw; wr32(out, w); wr32(out + 4, (uint32_t)((uint64_t)w * ih / iw)); return; }
    if (!held_w) { held_w = w; want_dir = 0; return; }
    int dir = w < held_w ? -1 : w > held_w ? 1 : 0;
    int64_t now = plat_qpc(), ms = plat_qpf() / 1000;
    if (dir != want_dir) { want_dir = dir; want_at = now; }
    if (dir && now - want_at >= (dir < 0 ? RES_DOWN_MS : g_up_ms) * ms) {
        uint32_t next = dir < 0 ? (held_w > RES_STEP ? held_w - RES_STEP : held_w) : held_w + RES_STEP;
        if (dir < 0 && next < w) next = w;
        if (dir > 0 && next > w) next = w;
        if (dir < 0 && up_at && now - up_at < RES_FAILED_MS * ms) g_up_ms = g_up_ms * 2 < RES_UP_MAX_MS ? g_up_ms * 2 : RES_UP_MAX_MS;      /* the last step up did not hold */
        if (dir > 0) up_at = now;
        held_w = next; want_dir = 0; want_at = now;
        if (++changes <= 300) { printf("smo_quality: render size now %u wide (change %lld, next step up after %d s of room)\n", held_w, changes, g_up_ms / 1000); fflush(stdout); }
    }
    wr32(out, held_w); wr32(out + 4, (uint32_t)((uint64_t)held_w * ih / iw));
}

void __wrap_f_4000a58394(Ctx *c) {
    uint64_t out = c->x[1], in = c->x[2], view = c->x[3];
    if (g_full < 0) {
        const char *e = getenv("SMO_DYNRES");
        g_full = e ? !atoi(e) : !gfx_options()->dynres;
        printf("smo_quality: dynamic_resolution = %s\n", g_full ? "off: the 3D is drawn at the full size the game asks for" : "on: the game caps its 3D size by its quality setting");
    }
    __real_f_4000a58394(c);
    static int nlog; static uint32_t last_w, last_h;
    if (nlog < 300 && !rd32(view + 4) && (rd32(out) != last_w || rd32(out + 4) != last_h)) {      /* 0.1.7: when it changes */
        nlog++; last_w = rd32(out); last_h = rd32(out + 4);
        printf("smo_quality: render size asked %ux%u, the game's cap gives %ux%u%s\n", rd32(in), rd32(in + 4), rd32(out), rd32(out + 4), g_full ? " (cap not applied)" : "");
        fflush(stdout);
    }
    if (g_full && !rd32(view + 4)) { wr32(out, rd32(in)); wr32(out + 4, rd32(in + 4)); }
    else if (!rd32(view + 4)) res_hold(out, in);
}
