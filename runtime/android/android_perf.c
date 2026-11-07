/* android_perf.c - Android's performance hints (ADPF, APerformanceHint, Android 13+) for the game's threads.
 *
 * Measured on a Galaxy S24 Ultra: no thread of the game is busy all the time (render 55%, the game's main thread 45%,
 * its helpers 7-21%: they wait for one another), so the CPU governor kept their clocks low and every step of a frame ran
 * slowly; the phone's own "performance" game mode made a clear difference in play. A hint session asks for the clocks
 * a frame needs instead, on any phone: the render thread and the game's threads are its threads, the target is one
 * frame at 60 fps, and after each present the time since the present before is reported. Frames that take longer make
 * the system raise those threads' clocks; frames on time let it lower them again.
 *
 * The functions are looked up at run time (libandroid.so of Android 13+; this library is built for Android 11), so an
 * older system simply has no session. SMO_PERF_HINT=0: none. Android port (2026-10-29). */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <sched.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/types.h>
#include <android/thermal.h>
#include "gfx_options.h"
#include "plat.h"

const char *plat_data_root(void);                    /* plat.h */

typedef struct APerformanceHintManager APerformanceHintManager;
typedef struct APerformanceHintSession APerformanceHintSession;
static APerformanceHintManager *(*p_get_manager)(void);
static APerformanceHintSession *(*p_create)(APerformanceHintManager *, const int32_t *, size_t, int64_t);
static int (*p_report)(APerformanceHintSession *, int64_t);
static int (*p_set_threads)(APerformanceHintSession *, const pid_t *, size_t);        /* Android 14+ */
static void (*p_close)(APerformanceHintSession *);
static int (*p_prefer_efficiency)(APerformanceHintSession *, int);                       /* Android 15+ */
static int (*p_workload_increase)(APerformanceHintSession *, int, int, const char *);   /* Android 16+ */

#define PERF_MAX_TIDS 64
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int32_t g_tids[PERF_MAX_TIDS];
static int g_ntids, g_dirty, g_state = -1;         /* g_state: -1 not looked up, 0 off, 1 on */
static APerformanceHintSession *g_session;
static int64_t g_target_ns = 16666667;           /* SMO_PERF_TARGET_US */
static int g_boost;                               /* SMO_PERF_BOOST=1: no power-efficiency preference, a workload rise each frame */

static int perf_on(void) {
    if (g_state >= 0) return g_state;
    const char *e = getenv("SMO_PERF_HINT");
    g_state = 0;
    if (e && !atoi(e)) { printf("android_perf: performance hints off (SMO_PERF_HINT=0)\n"); return 0; }
    void *lib = dlopen("libandroid.so", RTLD_NOW);
    if (lib) {
        p_get_manager = (APerformanceHintManager *(*)(void))dlsym(lib, "APerformanceHint_getManager");
        p_create = (APerformanceHintSession *(*)(APerformanceHintManager *, const int32_t *, size_t, int64_t))dlsym(lib, "APerformanceHint_createSession");
        p_report = (int (*)(APerformanceHintSession *, int64_t))dlsym(lib, "APerformanceHint_reportActualWorkDuration");
        p_set_threads = (int (*)(APerformanceHintSession *, const pid_t *, size_t))dlsym(lib, "APerformanceHint_setThreads");
        p_close = (void (*)(APerformanceHintSession *))dlsym(lib, "APerformanceHint_closeSession");
        p_prefer_efficiency = (int (*)(APerformanceHintSession *, int))dlsym(lib, "APerformanceHint_setPreferPowerEfficiency");
        p_workload_increase = (int (*)(APerformanceHintSession *, int, int, const char *))dlsym(lib, "APerformanceHint_notifyWorkloadIncrease");
    }
    if ((e = getenv("SMO_PERF_TARGET_US")) && atoll(e) > 0) g_target_ns = atoll(e) * 1000;
    g_boost = (e = getenv("SMO_PERF_BOOST")) && atoi(e);
    g_state = p_get_manager && p_create && p_report && p_close;
    printf("android_perf: performance hints %s\n", g_state ? "available (APerformanceHint): the game's threads get the clocks a 60 fps frame needs" : "not on this system (Android 13+ has them)");
    if (g_state) printf("android_perf: target %.2f ms, SMO_PERF_BOOST %s (power-efficiency call %s, workload-rise call %s)\n", g_target_ns / 1e6,
                        g_boost ? "on" : "off", p_prefer_efficiency ? "there" : "missing", p_workload_increase ? "there" : "missing");
    return g_state;
}

/* A thread of the game (render thread, guest threads) joins the session; from its own thread. */
void android_perf_add_thread(void) {
    int32_t tid = (int32_t)gettid();
    pthread_mutex_lock(&g_lock);
    int have = 0;
    for (int i = 0; i < g_ntids; i++) have |= g_tids[i] == tid;
    if (!have && g_ntids < PERF_MAX_TIDS) { g_tids[g_ntids++] = tid; g_dirty = 1; }
    pthread_mutex_unlock(&g_lock);
}

/* SMO_SPIN=<cpu>,<cpu>...: a measuring aid, not for play. One thread per CPU named, held on it and kept busy, so the
 * governor runs that CPU's cluster at its highest clock: the frame rate with full clocks, against the frame rate without.
 * They start when <files>/config/spin_now appears (a test script makes it once in gameplay), so the menus before do
 * not heat the phone and the comparison is at the same temperature. */
static void *spin_main(void *arg) {
    int cpu = (int)(intptr_t)arg;
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    sched_setaffinity(0, sizeof set, &set);
    for (volatile uint64_t n = 0;; n++) { }
    return NULL;
}
static void spin_start(void) {
    const char *e = getenv("SMO_SPIN");
    for (const char *p = e; p && *p; ) {
        int cpu = atoi(p);
        pthread_t t;
        if (pthread_create(&t, NULL, spin_main, (void *)(intptr_t)cpu) == 0) { pthread_detach(t); printf("android_perf: SMO_SPIN: CPU %d kept busy (a measuring aid)%c", cpu, 10); }
        p = strchr(p, ',');
        if (p) p++;
    }
}

/* Every 10 s of play a "perf:" line in log.txt, for reports from other phones (Export logs in the launcher): the frame
 * rate, the 1% low, the worst frame, the frame mode and scale, each CPU cluster's clock and Android's thermal state. */
static int cmp_float(const void *a, const void *b) { float x = *(const float *)a, y = *(const float *)b; return (x > y) - (x < y); }
static void perf_line(int64_t dt) {
    enum { MAXF = 4096 };
    static float ms[MAXF];
    static int nf;
    static double total;
    static AThermalManager *thermal;
    static int thermal_tried;
    if (dt <= 0 || dt >= 1000000000) return;           /* a second or more: a pause (the app in the background), not a frame */
    if (nf < MAXF) ms[nf++] = (float)(dt / 1e6);
    total += dt / 1e6;
    if (total < 10000.0 || nf < 2) return;
    double sum = 0;
    for (int i = 0; i < nf; i++) sum += ms[i];
    qsort(ms, (size_t)nf, sizeof ms[0], cmp_float);
    int slow = nf / 100 ? nf / 100 : 1;                /* the 1% low: the average of the slowest 1% of the frames */
    double slow_sum = 0;
    for (int i = nf - slow; i < nf; i++) slow_sum += ms[i];
    float worst = ms[nf - 1];
    char cpu[256] = "";
    size_t at = 0;
    for (int i = 0; i < 16 && at < sizeof cpu - 32; i++) {      /* one cpufreq policy per cluster */
        char path[96];
        snprintf(path, sizeof path, "/sys/devices/system/cpu/cpufreq/policy%d/scaling_cur_freq", i);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        long khz = 0;
        if (fscanf(f, "%ld", &khz) == 1) at += (size_t)snprintf(cpu + at, sizeof cpu - at, "%scpu%d %ld", at ? ", " : "", i, khz / 1000);
        fclose(f);
    }
    if (!thermal_tried) { thermal_tried = 1; thermal = AThermal_acquireManager(); }
    static const char *k_thermal[] = {"none", "light", "moderate", "severe", "critical", "emergency", "shutdown"};
    int ts = thermal ? (int)AThermal_getCurrentThermalStatus(thermal) : -1;
    const GfxOptions *o = gfx_options();
    char mode[32];
    if (o->fps30) snprintf(mode, sizeof mode, "30 fps mode");
    else if (o->fps_cap) snprintf(mode, sizeof mode, "cap %d", o->fps_cap);
    else snprintf(mode, sizeof mode, "60 fps mode");
    char scale[16];                                    /* resolution_scale = auto reads as 0 */
    if (o->scale > 0) snprintf(scale, sizeof scale, "%.2f", o->scale); else snprintf(scale, sizeof scale, "auto");
    int gpu_mhz = 0;                                   /* Adreno: readable by an app. On a Galaxy S24 Ultra the clock decides the frame rate (nvn_vk_draw.c, submit_chunk) */
    {   FILE *f = fopen("/sys/class/kgsl/kgsl-3d0/clock_mhz", "r");
        if (f) { if (fscanf(f, "%d", &gpu_mhz) != 1) gpu_mhz = 0; fclose(f); } }
    char gpu[32] = "";
    if (gpu_mhz > 0) snprintf(gpu, sizeof gpu, "; GPU %d MHz", gpu_mhz);
    printf("perf: %.1f fps, 1%% low %.1f, worst frame %.1f ms over %d frames (%s, scale %s); CPU MHz %s; thermal %s%s%c",
           nf * 1000.0 / sum, 1000.0 * slow / slow_sum, worst, nf, mode, scale, at ? cpu : "unknown",
           ts >= 0 && ts <= 6 ? k_thermal[ts] : "unknown", gpu, 10);
    nf = 0;
    total = 0;
}

/* SMO_SPIN_TEST=1, a measuring aid: <files>/config/spinhot holds the hot threads' spin in microseconds (plat.h
 * plat_spinhot_ns), read every 60 frames; no file: 0. (A utilization floor through sched_setattr UTIL_CLAMP_MIN was
 * tried first: Android refuses it to apps, EPERM for every thread, 2026-11-03.) */
static void spin_test_poll(void) {
    static int on = -1, frames;
    static long cur = -1;
    if (on < 0) on = getenv("SMO_SPIN_TEST") && atoi(getenv("SMO_SPIN_TEST"));
    if (!on || ++frames % 60) return;
    char path[512];
    snprintf(path, sizeof path, "%s/config/spinhot", plat_data_root());
    long us = 0;
    FILE *f = fopen(path, "r");
    if (f) { if (fscanf(f, "%ld", &us) != 1) us = 0; fclose(f); }
    if (us < 0) us = 0;
    if (us == cur) return;
    plat_spinhot_set_ns((int64_t)us * 1000);
    printf("android_perf: hot threads spin %ld us before they block (SMO_SPIN_TEST)%c", us, 10);
    cur = us;
}

/* graphics.ini boost = auto (the default): the hot threads (plat_spin_mark_hot: render thread, the game thread that
 * presents) spin before they block while the frames are late, and do not when they are on time. Measured in Cascade's
 * waterfall view (1,300 draws, 30 fps mode, S24 Ultra): the governor kept the mid cores at 0.5-1.4 GHz and the frame at
 * 25.5 fps; a 16 ms spin raised them to 1.8-2.0 GHz and the frame to 29.3-29.8 fps, but the chip warmed ~2 C a minute
 * where it cooled ~2 C without, so it runs only where it is needed (2026-11-03).
 * One-second windows of frames (a frame over 100 ms, a loading pause, is left out): two windows in a row whose average
 * frame is 5% over the target (frame_rate 30: 33.3 ms, fps_cap: its frame, else 16.7 ms) turn it on, for at least 10 s;
 * then it goes off to see whether the frames keep up without it. A view that is still heavy turns it back on within
 * seconds, and then it stays on twice as long (at most 2 minutes) before the next look. boost = on: always on.
 * SMO_SPINHOT_US or SMO_SPIN_TEST set: the spin they give, nothing automatic. */
#define BOOST_SPIN_NS   16000000
#define BOOST_HOLD_NS   10000000000LL
#define BOOST_HOLD_MAX  120000000000LL
static void boost_frame(int64_t now, int64_t dt) {
    static int mode = -1, on, slow_run;
    static int64_t target, win_sum, win_start, hold = BOOST_HOLD_NS, until, off_at;
    static int win_n;
    if (mode < 0) {
        const GfxOptions *o = gfx_options();
        mode = o->boost && !getenv("SMO_SPINHOT_US") && !(getenv("SMO_SPIN_TEST") && atoi(getenv("SMO_SPIN_TEST")));
        target = o->fps30 ? 33333333 : o->fps_cap ? 1000000000LL / o->fps_cap : 16666667;
        if (mode && o->boost == 2) mode = 2;
        printf("android_perf: CPU boost %s%c", mode == 2 ? "on (graphics.ini boost = on: the frame's threads always spin)" : mode ? "auto (the frame's threads spin while the frames are late)" : "off", 10);
        if (mode) plat_spinhot_set_ns(mode == 2 ? BOOST_SPIN_NS : 0);
    }
    if (mode != 1 || dt <= 0 || dt > 100000000) return;
    if (!win_n) win_start = now;
    win_sum += dt;
    win_n++;
    if (now - win_start < 1000000000) return;
    int slow = win_sum > (int64_t)win_n * target / 100 * 105;
    double fps = win_n * 1e9 / (double)win_sum;
    win_sum = 0;
    win_n = 0;
    if (on) {
        if (now < until) return;
        on = 0;
        slow_run = 0;
        off_at = now;
        plat_spinhot_set_ns(0);
        printf("android_perf: CPU boost off, %.1f fps with it (a look whether the frames keep up without)%c", fps, 10);
        return;
    }
    slow_run = slow ? slow_run + 1 : 0;
    if (slow_run < 2) return;
    hold = off_at && now - off_at < 8000000000LL ? (hold * 2 > BOOST_HOLD_MAX ? BOOST_HOLD_MAX : hold * 2) : BOOST_HOLD_NS;
    on = 1;
    until = now + hold;
    plat_spinhot_set_ns(BOOST_SPIN_NS);
    printf("android_perf: CPU boost on for %.0f s, %.1f fps without it%c", hold / 1e9, fps, 10);
}

/* After a present, on the render thread: the time since the present before. */
void android_perf_frame(void) {
    spin_test_poll();
    static int64_t last_ns;
    static int spun, frames;
    if (!spun && getenv("SMO_SPIN") && ++frames % 30 == 0) {
        char path[512];
        snprintf(path, sizeof path, "%s/config/spin_now", plat_data_root());
        if (access(path, F_OK) == 0) { spun = 1; spin_start(); }
    }
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    int64_t now = (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec, dt = last_ns ? now - last_ns : 0;
    last_ns = now;
    perf_line(dt);
    boost_frame(now, dt);
    if (!perf_on()) return;
    pthread_mutex_lock(&g_lock);
    if (g_dirty && g_ntids) {                      /* threads came (or the first frame): the session's thread list */
        g_dirty = 0;
        int n = 0;                                 /* only threads that still run: the system refuses a list with a thread that has ended */
        for (int i = 0; i < g_ntids; i++) {
            char path[64];
            snprintf(path, sizeof path, "/proc/self/task/%d", (int)g_tids[i]);
            if (access(path, F_OK) == 0) g_tids[n++] = g_tids[i];
        }
        g_ntids = n;
        if (g_session && p_set_threads && p_set_threads(g_session, (const pid_t *)g_tids, (size_t)g_ntids) == 0) {}
        else if (g_ntids) {
            APerformanceHintManager *m = p_get_manager();
            APerformanceHintSession *ns = m ? p_create(m, g_tids, (size_t)g_ntids, g_target_ns) : NULL;
            if (ns) { if (g_session) p_close(g_session); g_session = ns; if (g_boost && p_prefer_efficiency) p_prefer_efficiency(ns, 0); }      /* refused: the session before stays */
            static int said;
            if (!said++ || !ns) printf("android_perf: hint session for %d threads %s\n", g_ntids, ns ? "made" : "REFUSED (the one before stays)");
        }
    }
    APerformanceHintSession *s = g_session;
    pthread_mutex_unlock(&g_lock);
    if (s && dt > 0) p_report(s, dt > 100000000 ? 100000000 : dt);      /* at most 100 ms: a loading pause is no frame */
    if (s && g_boost && p_workload_increase) p_workload_increase(s, 1, 0, "frame");
}
