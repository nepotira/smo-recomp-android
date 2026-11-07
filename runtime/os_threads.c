/* nn::os threading and synchronisation on top of Win32.
 *
 * Guest synchronisation objects (MutexType, EventType, ...) are opaque here:
 * each one is identified by its guest address and backed by a host object
 * kept in a side table, so the guest layout never has to be known. */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include "plat.h"
#include "runtime.h"
#include "hostrt.h"

#define A0 (c->x[0])
#define A1 (c->x[1])
#define A2 (c->x[2])
#define A3 (c->x[3])
#define A4 (c->x[4])
#define RET(v) (c->x[0] = (uint64_t)(v))
#define TICK_HZ 19200000ULL

typedef struct Obj {
    uint64_t key;
    struct Obj *next;
    plat_mutex cs;
    plat_cond cv;
    int signaled, autoclear;                 /* events */
    int count, max;                          /* semaphores */
    uint64_t *buf, cap, len, head;           /* message queues */
    plat_thread *thread;                     /* threads */
    uint64_t fn, arg, stack, stack_size, name;
    int priority, finished;
    void *fiber;                             /* fibers */
    uint64_t tp;                             /* fibers: thread block (tpidr) of the host thread that runs the fiber now */
    int ideal_core, core_set;                /* threads: nn::os ideal core and affinity mask as the game set them */
    uint64_t core_mask;
    uint64_t target, user;                   /* multi-wait holders */
    struct Obj *holders[64];                 /* multi-wait */
    int holder_count;
    struct Obj *alias;                       /* SMO port: system event attached by handle: the event object it shares */
    uint32_t rhandle, whandle;               /* SMO port: kernel-style handles of a system event created here */
    uint64_t sig_by; int64_t sig_t;          /* SMO_WAITMAP: the thread that signalled (or unlocked) it last, and when */
} Obj;

#define BUCKETS 65536
static Obj *g_table[BUCKETS];
static plat_rwlock g_table_lock = PLAT_RWLOCK_INIT;

static Obj *obj(uint64_t key) {
    unsigned h = (unsigned)((key >> 3) * 2654435761u) & (BUCKETS - 1);
    plat_rw_wrlock(&g_table_lock);
    Obj *o = g_table[h];
    while (o && o->key != key) o = o->next;
    if (!o) {
        o = calloc(1, sizeof *o);
        o->key = key;
        plat_mutex_init(&o->cs);
        plat_cond_init(&o->cv);
        o->next = g_table[h];
        g_table[h] = o;
    }
    plat_rw_wrunlock(&g_table_lock);
    return o->alias ? o->alias : o;            /* SMO port: a system event attached by handle shares its event */
}
/* SMO port: the object itself, without following an alias (its own clear mode) */
static Obj *obj_raw(uint64_t key) {
    Obj *o = obj(key);
    unsigned h = (unsigned)((key >> 3) * 2654435761u) & (BUCKETS - 1);
    plat_rw_rdlock(&g_table_lock);
    Obj *r = g_table[h];
    while (r && r->key != key) r = r->next;
    plat_rw_rdunlock(&g_table_lock);
    return r ? r : o;
}
/* Forget an object so a new one created at the same address starts clean. */
static void obj_reset(uint64_t key) {
    Obj *o = obj(key);
    plat_mutex_lock(&o->cs);
    o->signaled = o->autoclear = o->count = o->max = 0;
    o->len = o->head = 0;
    o->holder_count = 0;
    plat_mutex_unlock(&o->cs);
}

static uint32_t ns_to_ms(int64_t ns) { return ns <= 0 ? 0 : (uint32_t)((ns + 999999) / 1000000); }

/* nn::os::SleepThread takes nanoseconds and the console's timer is good to a few microseconds. plat_sleep_ms() counts whole
 * milliseconds on a 1 ms (or 15.6 ms) grid, so the wait runs on a high-resolution waitable timer (100 ns units,
 * Windows 10 1803 and later); plat_sleep_ms() is only the fallback. A zero or negative span yields, as on the console. */
static void sleep_ns(int64_t ns) {
#ifndef _WIN32
    if (ns <= 0) { plat_yield(); return; }
    plat_sleep_ns(ns);
    return;
#else
    static _Thread_local HANDLE timer;
    static _Thread_local int no_timer;
    plat_li due;
    if (ns <= 0) { plat_yield(); return; }
    if (!timer && !no_timer) {
        timer = CreateWaitableTimerExW(NULL, NULL, 0x00000002 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
        if (!timer) no_timer = 1;
    }
    due.QuadPart = -((ns + 99) / 100);
    /* Session 8 (track V), SMO_SLEEP_LOG=1: a line for every sleep that lasted 2 ms longer than asked (first 200). */
    static int lg = -1; static int32_t nl;
    if (lg < 0) lg = getenv("SMO_SLEEP_LOG") && atoi(getenv("SMO_SLEEP_LOG"));
    plat_li q0, q1, fq;
    if (lg) ((q0).QuadPart = plat_qpc());
    if (!timer || !SetWaitableTimer(timer, &due, 0, NULL, NULL, 0)) plat_sleep_ms(ns_to_ms(ns));
    else WaitForSingleObject(timer, PLAT_INFINITE);
    if (lg) {
        extern long long nvn_front_frame_next(void);
        ((q1).QuadPart = plat_qpc()); ((fq).QuadPart = plat_qpf());
        double ms = (double)(q1.QuadPart - q0.QuadPart) * 1000.0 / (double)fq.QuadPart, asked = (double)ns / 1e6;
        {   /* per thread and frame: how many sleeps, asked and slept in all; a line when a frame after 1900 had over 8 ms of them */
            static _Thread_local long long fr0, cnt, zero; static _Thread_local double a_sum, s_sum, a_max; static int32_t nf;
            long long fr = nvn_front_frame_next();
            if (fr != fr0) {
                if (fr0 > 1900 && s_sum > 8.0 && plat_atomic_inc(&nf) <= 300) printf("sleep_log: frame %lld, thread %lu: %lld sleeps in the frame, asked %.3f ms in all (longest asked %.3f), slept %.3f ms\n", fr0, plat_tid(), cnt, a_sum, a_max, s_sum);
                fr0 = fr; cnt = 0; a_sum = s_sum = a_max = 0;
            }
            cnt++; a_sum += asked; s_sum += ms; if (asked > a_max) a_max = asked;
        }
        if (ms > asked + 2.0 && plat_atomic_inc(&nl) <= 600) { printf("sleep_log: frame %lld, thread %lu: asked %.3f ms, slept %.3f ms (%s)\n", nvn_front_frame_next(), plat_tid(), asked, ms, timer ? "high-resolution timer" : "Sleep"); fflush(stdout); }
    }
#endif
}

/* nn::os thread priorities run from 0 (highest) to 31 (lowest), 16 is the default; the application's main thread
 * starts at the priority of its NPDM (44 - 28 = 16 in nn::os terms). Windows has seven levels per process class:
 * the order is kept, the spacing is not. */
#define OS_DEFAULT_PRIORITY 16
#define OS_DEFAULT_CORE 0                    /* main.npdm: default core id 0 */
/* Console scheduling model, OPT-IN: SMO_OS_CONSOLE_CORES=1 (bounded round 2026-10-09). What the game asks of nn::os in a
 * match (os_thread_report, the list at the end of every run): every thread is created on ONE core of the console's
 * three (mask 1, 2 or 4) except four with mask 7, at the priorities 1, 8, 9, 15, 16, 23 and 31, and the console runs on
 * each core only the highest-priority ready thread of that core. With the switch on:
 *   - three host processors stand for the console's cores 0..2 and every guest thread's affinity is its nn::os core mask
 *     mapped onto them (set at StartThread and at every SetThreadCoreMask), so two threads the console keeps on one core
 *     never run at the same time here either;
 *   - six priority levels instead of five (<= 4, 5..8, 9..15, 16, 17, 18..31), Windows' dynamic priority boost off:
 *     the order of the seven values ssbu uses is kept except 9 against 15. SMO uses 0, 7, 8, 15, 16, 17, 18, 19, 20 and
 *     22; what must stay apart is 17 against 18 on core 2: the model draw buffer update (描画バッファ更新, 17) that every
 *     scene starts at the end of a frame and waits for at the start of the next, and the archive decompression
 *     (DecompThread, 18) busy while a stage loads. At one level the update waited behind the decompression for a whole
 *     time slice, long enough for the scene being left to be deleted under it (HakoniwaStateDeleteScene runs a frame or
 *     two after the scene's last frame): "pure virtual function called" in the update's list of actors on entering a
 *     stage from the title menu, 2 times in 91 runs of the stage stress test (tests/stress_stages.py, two games on the
 *     same three processors); with 18..31 a level lower, 0 times in 276 runs of it. 17 is the last level above
 *     Windows' lowest;
 *   - GetCurrentCoreNumber / GetCurrentProcessorNumber answer the core the thread is really on.
 * NOT the console's scheduler all the same: Windows time-slices equal priorities, other processes of the PC take the
 * three processors too, and host-side threads (graphics driver, audio device, movie decoding) are not part of it.
 * ssbu-recomp had it off by default (three pinned processors cost frame rate on a busy PC).
 * SMO port: ON BY DEFAULT (SMO_OS_CONSOLE_CORES=0 turns it off). Measured 2026-10-15, 20 s boots, 6 runs each: with free
 * scheduling 4 of 6 runs failed in sead heap code on the loading path (sead::ExpHeap::tryAlloc failing inside
 * ParallelSZSDecompressor, or a sead::Heap destructor reading address -1); with the console core model 0 of 6 failed.
 * SMO depends on its threads being scheduled the console's way, so that is the default.
 * The three processors are taken from the top of a slot of six (three cores with their second hardware threads): every
 * game running at once on the PC takes the first slot no other game holds (a named mutex per slot, held while the
 * process lives), so games side by side (tests, or a test beside the copy being played) do not share processors. One
 * game alone takes slot 0, the top of the affinity mask: the processors it always had. Before the slots, four test
 * games at once all ran their console cores on the same three processors. SMO_OS_HOST_CPUS=<hexadecimal mask> chooses
 * from those processors instead (and takes no slot). */
static int g_cc = -1;
static uintptr_t g_cc_core[3];
static uint64_t g_render_cpu;                    /* Android port: the processor kept for the render thread (0: none) */
static int console_cores(void);
uint64_t os_render_cpu_mask(void) { console_cores(); return g_render_cpu; }      /* nvn_gl.c render_thread */
#ifdef _WIN32
static uintptr_t core_slot(uintptr_t proc) {           /* the first free slot of six processors from the top of proc */
    int bits[64], nb = 0;
    for (int b = (int)(sizeof(uintptr_t) * 8) - 1; b >= 0; b--) if (proc >> b & 1) bits[nb++] = b;
    for (int k = 0; k < nb / 6; k++) {
        char name[64];
        snprintf(name, sizeof name, "Local\\smo_console_cores_%d", k);
        HANDLE m = CreateMutexA(NULL, 0, name);     /* kept open: the slot is this process's until it ends */
        if (!m) break;
        if (GetLastError() == ERROR_ALREADY_EXISTS) { CloseHandle(m); continue; }
        uintptr_t mask = 0;
        for (int i = 6 * k; i < 6 * k + 6; i++) mask |= (uintptr_t)1 << bits[i];
        printf("nn::os: console cores in processor slot %d of %d (mask %llx; other games running at once take other slots)\n", k, nb / 6, (unsigned long long)mask);
        return mask;
    }
    printf("nn::os: every processor slot is held by other games: sharing the top of the mask\n");
    return proc;
}
#else
/* Android port: phones mix fast and slow cores. The three console cores go to the three processors with the highest
 * capacity (cpu_capacity, else the highest maximum frequency) among those the process may run on; equal ones by the
 * higher number (the fast cluster is numbered last on every SoC seen). Hardware threads are not a concern there. */
static uint64_t cpu_rank(int cpu) {
    char path[128];
    unsigned long long v = 0;
    snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/cpu_capacity", cpu);
    FILE *f = fopen(path, "r");
    if (!f) { snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", cpu); f = fopen(path, "r"); }
    if (f) { if (fscanf(f, "%llu", &v) != 1) v = 0; fclose(f); }
    return v;
}
static int pick_console_cpus(uint64_t allowed, int out[3]) {
    int n = 0;
    uint64_t used = 0;
    for (int k = 0; k < 3; k++) {
        int best = -1;
        uint64_t best_rank = 0;
        for (int cpu = 63; cpu >= 0; cpu--) {
            if (!(allowed >> cpu & 1) || used >> cpu & 1) continue;
            uint64_t r = cpu_rank(cpu);
            if (best < 0 || r > best_rank) { best = cpu; best_rank = r; }
        }
        if (best < 0) break;
        used |= 1ull << best;
        out[n++] = best;
    }
    return n;
}
#endif
static int console_cores(void) {
    if (g_cc < 0) {
        int n = 0, on = 0;
        const char *cc = getenv("SMO_OS_CONSOLE_CORES"), *cpus = getenv("SMO_OS_HOST_CPUS");
        if (!cc || atoi(cc) != 0) {
            uintptr_t proc = 0, sys = 0;
#ifdef _WIN32
            GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys);
#else
            {   cpu_set_t set;
                CPU_ZERO(&set);
                sched_getaffinity(0, sizeof set, &set);
                for (int i = 0; i < 64; i++) if (CPU_ISSET(i, &set)) proc |= (uintptr_t)1 << i;
                (void)sys; }
#endif
#ifdef __ANDROID__
            /* Android port: the fastest processor is kept for the render thread (os_render_cpu_mask), which replays every draw
             * and was the bottleneck; the console's three cores take the next three (measured on a Galaxy S24 Ultra, opening
             * cutscene: 23 -> 26 fps with the render thread on the Cortex-X4). SMO_OS_HOST_CPUS overrides. */
            if (!(cpus && cpus[0])) {
                int best = -1; uint64_t br = 0, nproc = 0;
                for (int c = 0; c < 64; c++) if (proc >> c & 1) { uint64_t r = cpu_rank(c); nproc++; if (best < 0 || r > br) { best = c; br = r; } }
                if (best >= 0 && nproc >= 6) { g_render_cpu = (uint64_t)1 << best; proc &= ~(uintptr_t)g_render_cpu; }
            }
#endif
            if (cpus && cpus[0]) {
                uintptr_t want = (uintptr_t)strtoull(cpus, NULL, 16);
                printf("nn::os: SMO_OS_HOST_CPUS=%s: the console's cores on these processors only (%llx of %llx)\n", cpus, (unsigned long long)(want & proc), (unsigned long long)proc);
                if (want & proc) proc &= want;
            }
#ifdef _WIN32
            else proc = core_slot(proc);
            for (int b = (int)(sizeof(uintptr_t) * 8) - 1; b >= 0 && n < 3; b--)      /* from the top, every second processor: not two hardware threads of one core */
                if (proc >> b & 1) { g_cc_core[n++] = (uintptr_t)1 << b; b--; }
#else
            {   int cpu[3];
                n = pick_console_cpus(proc, cpu);
                for (int i = 0; i < n; i++) g_cc_core[i] = (uintptr_t)1 << cpu[i]; }
#endif
            on = n == 3;
            printf("nn::os: console core model %s (SMO_OS_CONSOLE_CORES): host processor masks %llx %llx %llx for cores 0 1 2\n", on ? "ON" : "NOT possible (fewer than 3 usable processors)",
                   (unsigned long long)g_cc_core[0], (unsigned long long)g_cc_core[1], (unsigned long long)g_cc_core[2]);
        }
        g_cc = on;
    }
    return g_cc;
}
/* t NULL with self set: the calling thread; t NULL without self: a thread not started yet (applied when it starts) */
static void apply_sched(plat_thread *t, int self, uint64_t core_mask) {
    uintptr_t m = 0;
    if (!console_cores() || (!t && !self)) return;
    for (int i = 0; i < 3; i++) if (core_mask >> i & 1) m |= g_cc_core[i];
    plat_thread_set_affinity(t, m ? m : g_cc_core[0]);
}
static int host_priority(int prio) {
    static int split = -1;                                     /* TEST ONLY, SMO_OS_PRIO17=0: 17 shares its level with 18..27 again */
    if (split < 0) { const char *e = getenv("SMO_OS_PRIO17"); split = !e || atoi(e) != 0; }
    if (console_cores()) {
        if (prio <= 4) return PLAT_PRIO_TIME_CRITICAL;
        if (prio <= 8) return PLAT_PRIO_HIGHEST;
        if (prio >= 18 && (split || prio >= 28)) return PLAT_PRIO_LOWEST;     /* below the draw buffer update (17), see above */
    }
    if (prio <= 4) return PLAT_PRIO_HIGHEST;
    if (prio <= 15) return PLAT_PRIO_ABOVE;
    if (prio == 16) return PLAT_PRIO_NORMAL;
    if (prio <= 27) return PLAT_PRIO_BELOW;
    return PLAT_PRIO_LOWEST;
}

/* ---------------------------------------------------------------- threads */
static _Thread_local uint64_t t_thread;          /* guest ThreadType* of the running thread */
static _Thread_local uint64_t t_tls[64];
static uint64_t g_tls_dtor[64];                       /* guest destructor of each slot, 0 = none */
static volatile int32_t g_tls_used[64];
static void tls_run_destructors(Ctx *c);
static _Thread_local uint64_t t_fiber;           /* guest FiberType* currently running, 0 if none */
static _Thread_local void *t_thread_fiber;       /* host fiber of the thread itself */
static uint8_t g_main_thread[0x400];
uint64_t os_main_thread_type(void) { return (uint64_t)(uintptr_t)g_main_thread; }   /* boot.c: stored at thread block +0x1f8, as [sdk 0x3349dc] does */
uint64_t g_main_stack_base, g_main_stack_size;        /* set by boot.c */
static volatile int32_t g_thread_count;

static Ctx *new_ctx(uint64_t stack_top) {
    Ctx *c = plat_aligned_malloc(sizeof *c, 16);
    memset(c, 0, sizeof *c);
    c->x[31] = stack_top & ~15ull;
    c->tpidr = (uint64_t)(uintptr_t)plat_vm_alloc(NULL, 0x10000, PLAT_PROT_RW);
    return c;
}

/* Every guest thread's context, so a report can say where each one is. */
static struct { Obj *o; Ctx *c; } g_threads[256];
static volatile int32_t g_threads_n;

void dump_guest_threads(FILE *out) {
    fprintf(out, "\nguest threads (last function entered, link register):\n");
    for (int i = 0; i < g_threads_n && i < 256; i++) {
        Obj *o = g_threads[i].o;
        Ctx *c = g_threads[i].c;
        fprintf(out, "  %-20s %s  f_%llx  lr=%llx  %s%s (from %llx)\n", o->name ? (const char *)(uintptr_t)o->name : "?",
                o->finished ? "finished" : "running ", (unsigned long long)c->fn, (unsigned long long)c->x[30],
                c->in_import ? "in " : "", c->in_import ? c->in_import : "guest code", (unsigned long long)c->import_from);
    }
}

/* hitch.c (SMO_HITCH): guest thread i -> context, name, host handle; 0 past the end. */
int guest_thread_info(int i, Ctx **c, const char **name, plat_thread **h) {
    if (i < 0 || i >= g_threads_n || i >= 256 || !g_threads[i].o || !g_threads[i].c) return 0;
    *c = g_threads[i].c; *name = g_threads[i].o->name ? (const char *)(uintptr_t)g_threads[i].o->name : NULL; *h = g_threads[i].o->finished ? NULL : g_threads[i].o->thread;
    return 1;
}
void hitch_cur_ctx(Ctx *c);
/* Name of the guest thread running on host thread `tid`, or NULL (nvn_gl.c: CPU time per thread, P3). */
const char *guest_thread_name_of(uint32_t tid) {
    for (int i = 0; i < g_threads_n && i < 256; i++) {
        Obj *o = g_threads[i].o;
        if (o && o->thread && plat_thread_id(o->thread) == tid) return o->name ? (const char *)(uintptr_t)o->name : "?";
    }
    return NULL;
}

static void thread_main(void *arg) {
    Obj *o = arg;
    t_thread = o->key;
    plat_thread_set_priority(NULL, host_priority(o->priority));     /* the thread sets its own scheduling before any guest code */
    apply_sched(NULL, 1, o->core_mask);
#ifdef __ANDROID__
    { void android_perf_add_thread(void); android_perf_add_thread(); }     /* android_perf.c: the game's threads get performance hints */
#endif
    Ctx *c = new_ctx(o->stack + o->stack_size);
    wr64(c->tpidr + 0x1f8, o->key);                   /* Horizon thread-local region: the ThreadType pointer lives at +0x1f8 */
    int32_t slot = plat_atomic_inc(&g_threads_n) - 1;
    if (slot < 256) { g_threads[slot].o = o; g_threads[slot].c = c; }
    c->x[0] = o->arg;
    trace_printf("--- thread %llx started (entry %llx, name %s)\n", (unsigned long long)o->key, (unsigned long long)o->fn,
                 o->name ? (const char *)(uintptr_t)o->name : "?");
    call_guest(c, o->fn);
    tls_run_destructors(c);
    trace_printf("--- thread %llx finished\n", (unsigned long long)o->key);
    plat_mutex_lock(&o->cs);
    o->finished = 1;
    plat_cond_broadcast(&o->cv);
    plat_mutex_unlock(&o->cs);
}

/* SMO port: the fields of nn::os::ThreadType (SDK 3.5.1) that the game reads itself. [sdk 0x8a680]
 * detail::ThreadManager::CreateThread(this, ThreadType *t, fn, arg, stack, size, priority, core) stores priority at +0x44,
 * the stack at +0x48, the stack the thread RUNS on at +0x50 (a mapped alias of +0x48 made by CreateAliasStackUnsafe; here
 * the thread runs on the stack it was given, so the same address), the size at +0x58, arg at +0x60 and fn at +0x68.
 * sead::MemUtil::isStack (main 0xb5dad0) compares a pointer with [+0x50, +0x50 + size) of every sead thread:
 * al::createStringIfInStack (main 0x815f84) copies a string to the heap only when that says "on a stack" (the opening
 * demo's DemoActionList::initSerial keeps its action names this way). */
static void thread_type_fill(uint64_t t, uint64_t fn, uint64_t arg, uint64_t stack, uint64_t size, int priority) {
    if (!t) return;
    wr32(t + 0x44, (uint32_t)priority);
    wr64(t + 0x48, stack); wr64(t + 0x50, stack); wr64(t + 0x58, size);
    wr64(t + 0x60, arg); wr64(t + 0x68, fn);
}
static void os_CreateThread(Ctx *c) {                 /* (ThreadType*, fn, arg, stack, stackSize, priority[, core]) */
    thread_type_fill(A0, A1, A2, A3, A4, (int)c->x[5]);
    Obj *o = obj(A0);
    o->fn = A1; o->arg = A2; o->stack = A3; o->stack_size = A4; o->priority = (int)c->x[5];
    o->finished = 0; o->thread = NULL; o->name = 0;
    o->ideal_core = OS_DEFAULT_CORE; o->core_mask = 1ull << OS_DEFAULT_CORE; o->core_set = 1;
    RET(0);
}
static void os_CreateThreadOnCore(Ctx *c) {           /* (ThreadType*, fn, arg, stack, stackSize, priority, idealCore) */
    uint64_t t = A0;
    int core = (int)c->x[6];
    os_CreateThread(c);
    if (core >= 0 && core < 64) { Obj *o = obj(t); o->ideal_core = core; o->core_mask = 1ull << core; }
}
/* Evidence for the audit rows "nn::os objects" / "thread scheduling" (bounded round 2026-10-09): every thread the game
 * starts, with the nn::os arguments it was given, and every later change of priority or core mask. Printed at the end
 * of a run by os_thread_report (hostrt.c report_and_exit). */
static char g_oslog[24000];
static volatile int32_t g_oslog_n, g_oslog_lock;
static void oslog(const char *fmt, ...) {
    char line[320];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    if (n >= (int)sizeof line) n = (int)sizeof line - 1;
    while (plat_atomic_cas(&g_oslog_lock, 1, 0)) plat_cpu_relax();
    if (g_oslog_n + n < (int32_t)sizeof g_oslog - 1) { memcpy(g_oslog + g_oslog_n, line, (size_t)n); g_oslog_n += n; }
    g_oslog_lock = 0;
}
void os_thread_report(FILE *out) {
    if (!g_oslog_n) return;
    fprintf(out, "nn::os threads of this run (priority 0 highest .. 31 lowest; cores 0..2 exist on the console):\n%.*s", (int)g_oslog_n, g_oslog);
}
static void os_StartThread(Ctx *c) {
    Obj *o = obj(A0);
    oslog("  start  %-28s entry %llx priority %2d ideal core %d mask %llx stack 0x%llx (called from %llx)\n", o->name ? (const char *)(uintptr_t)o->name : "?",
          (unsigned long long)o->fn, o->priority, o->ideal_core, (unsigned long long)o->core_mask, (unsigned long long)o->stack_size, (unsigned long long)c->x[30]);
    plat_atomic_inc(&g_thread_count);
    o->thread = plat_thread_start(thread_main, o, (size_t)256 << 20, o->name ? (const char *)(uintptr_t)o->name : "guest");
}
/* The process runs in the ABOVE_NORMAL priority class (SMO_PRIORITY_CLASS=normal: the NORMAL class, as before). With the
 * console core model every guest thread may run on one host processor only, and the game's helpers at nn::os priority
 * 17..27 (view update, clipping, draw buffer update: the main thread waits for them every frame) are BELOW_NORMAL
 * threads: in the NORMAL class any normal thread of another program on one of those three processors comes first. One
 * class up every level of the mapping is two higher (the six levels stay apart: 8 9 10 11 12 15), above the normal
 * threads of other programs (8); HIGH would put HIGHEST and TIME_CRITICAL together. Measured at Cascade Kingdom's first
 * checkpoint: with up to 8 busy programs (11 of 32 processors busy) Windows keeps them off those three processors and
 * the class changes nothing; with every processor busy (30 busy programs) the game in the NORMAL class got 44% of one
 * processor and had not reached play after 120 s, in ABOVE_NORMAL it played at 38-39 fps. */
static void process_priority(void) {
    const char *e = getenv("SMO_PRIORITY_CLASS");
    if (e && !plat_stricmp(e, "normal")) { printf("nn::os: process priority class NORMAL (SMO_PRIORITY_CLASS=normal)\n"); return; }
#ifdef _WIN32
    int ok = SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);
#else
    int ok = 1;                                       /* no priority class: every level of host_priority is a nice value */
#endif
    printf("nn::os: process priority class ABOVE_NORMAL%s (SMO_PRIORITY_CLASS=normal for the NORMAL class)\n", ok ? "" : ": REFUSED");
}
/* boot.c, on the main guest thread before the game starts: main.npdm gives it core 0 and priority 44 - 28 = 16 */
void os_apply_main_sched(void) {
    process_priority();
    if (!console_cores()) return;
    apply_sched(NULL, 1, 1ull << OS_DEFAULT_CORE);
    plat_thread_set_priority(NULL, host_priority(OS_DEFAULT_PRIORITY));
}
static void os_WaitThread(Ctx *c) { Obj *o = obj(A0); if (o->thread) plat_thread_wait(o->thread, PLAT_INFINITE); }
static void os_DestroyThread(Ctx *c) { Obj *o = obj(A0); if (o->thread) { plat_thread_join(o->thread); o->thread = NULL; } }
static void os_SetThreadName(Ctx *c) {               /* the guest string may be a temporary: keep a copy */
    char *copy = strdup((const char *)(uintptr_t)A1);
    obj(A0)->name = (uint64_t)(uintptr_t)copy;
}
static void os_GetCurrentThread(Ctx *c) {
    if (!t_thread && !rd64((uint64_t)(uintptr_t)g_main_thread + 0x50) && g_main_stack_base)      /* the main thread's ThreadType, once its stack is known (boot.c) */
        thread_type_fill((uint64_t)(uintptr_t)g_main_thread, 0, 0, g_main_stack_base, g_main_stack_size, OS_DEFAULT_PRIORITY);
    RET(t_thread ? t_thread : (uint64_t)g_main_thread);
}
static Obj *thread_obj(uint64_t key);
static void os_GetThreadPriority(Ctx *c) { RET((int64_t)thread_obj(A0 == (uint64_t)(uintptr_t)g_main_thread ? 0 : A0)->priority); }
static void os_ChangeThreadPriority(Ctx *c) {
    Obj *o = obj(A0);
    int old = o->priority;
    oslog("  change priority of %s: %d -> %d (called from %llx)\n", A0 == (uint64_t)(uintptr_t)g_main_thread ? "the main thread" : o->name ? (const char *)(uintptr_t)o->name : "?", old, (int)A1, (unsigned long long)c->x[30]);
    o->priority = (int)A1;
    if (o->thread) plat_thread_set_priority(o->thread, host_priority(o->priority));
    else if (A0 == (uint64_t)(uintptr_t)g_main_thread || A0 == t_thread) plat_thread_set_priority(NULL, host_priority(o->priority));
    RET((int64_t)old);
}
static void os_GetThreadId(Ctx *c) { Obj *o = obj(A0); RET(o->thread ? plat_thread_id(o->thread) : plat_tid()); }
static void os_YieldThread(Ctx *c) { (void)c; plat_yield(); }
/* SMO_MOVIE_LOG: the movie player's pre-roll loop [game 0x71036f27b4] sleeps 10 ms up to 125 times until its video
 * renderer holds more than 1 frame (+0xb0) and its audio renderer has filled more than 8 / 16 buffers (+0x110). */
static void movie_preroll_diag(Ctx *c) {
    static int on = -1, n;
    if (on < 0) on = getenv("SMO_MOVIE_LOG") != NULL;
    if (!on || c->import_from != 0 /* SSBU 13.0.4 address */ || !SMO_UNPORTED || (n++ % 25)) return;
    uint64_t p = c->x[21], vr = rd64(p + 0x58) ? rd64(rd64(p + 0x58)) : 0, ar = rd64(p + 0x68) ? rd64(rd64(p + 0x68)) : 0;
    printf("movie: pre-roll sleep %d (loop count %u of %u): video frames queued %lld", n, (unsigned)c->x[24], (unsigned)c->x[25], vr ? (long long)rd64(vr + 0xb0) : -1LL);
    if (ar) printf("; audio: filled %lld, accumulate %u, pool %d buffers of %d bytes (flush at %d), free %lld, pending outputs %lld, +0x40 %d", (long long)rd64(ar + 0x110), rd8(ar + 0x50), (int)rd32(ar + 0x44), (int)rd32(ar + 0x48), (int)rd32(ar + 0x4c),
        (long long)((rd64(ar + 0x78) - rd64(ar + 0x70)) / 0x40), (long long)((rd64(ar + 0x60) - rd64(ar + 0x58)) / 0x18), (int)rd32(ar + 0x40));
    printf("%c", 10);
}
static void os_SleepThread(Ctx *c) { movie_preroll_diag(c); sleep_ns((int64_t)A0); }
static void os_GetCurrentStackInfo(Ctx *c) {
    Obj *o = t_thread ? obj(t_thread) : NULL;
    if (A0) wr64(A0, o ? o->stack : g_main_stack_base);
    if (A1) wr64(A1, o ? o->stack_size : g_main_stack_size);
}
/* Core numbers and affinity masks. The console runs the application on cores 0..2 (GetThreadAvailableCoreMask = 7);
 * a thread is created on one core (the NPDM default core 0, or the one given to CreateThread) and stays there until
 * SetThreadCoreMask widens its mask. The values are kept per thread and returned as set; the Windows scheduler is
 * not told to pin anything (a PC has more than three cores and the game's threads would pile up on host CPU 0). */
static Obj *thread_obj(uint64_t key) {
    Obj *o = obj(key ? key : (uint64_t)(uintptr_t)g_main_thread);
    if (!o->core_set) { o->core_set = 1; o->ideal_core = OS_DEFAULT_CORE; o->core_mask = 1ull << OS_DEFAULT_CORE; if (!o->priority) o->priority = OS_DEFAULT_PRIORITY; }
    return o;
}
static void os_SetThreadCoreMask(Ctx *c) {            /* (ThreadType*, int idealCore, Bit64 affinityMask) */
    Obj *o = thread_obj(A0);
    int ideal = (int)A1;
    uint64_t mask = A2;
    oslog("  core mask of %s: ideal %d mask %llx (was ideal %d mask %llx; %s; called from %llx)\n", A0 == (uint64_t)(uintptr_t)g_main_thread || !A0 ? "the main thread" : o->name ? (const char *)(uintptr_t)o->name : "?",
          ideal, (unsigned long long)mask, o->ideal_core, (unsigned long long)o->core_mask, o->thread ? "running" : "not started", (unsigned long long)c->x[30]);
    int is_self = t_thread ? A0 == t_thread : (!A0 || A0 == (uint64_t)(uintptr_t)g_main_thread);      /* the calling thread names itself */
    plat_thread *h = o->thread;                                                                         /* NULL and not self: not started yet (applied at StartThread) */
    int self = !h && is_self;
    if (ideal == -2) { o->ideal_core = OS_DEFAULT_CORE; o->core_mask = 1ull << OS_DEFAULT_CORE; apply_sched(h, self, o->core_mask); return; }   /* IdealCoreUseDefaultValue: mask is ignored */
    if (ideal == -3) { o->core_mask = mask; apply_sched(h, self, o->core_mask); return; }                           /* IdealCoreNoUpdate */
    o->core_mask = mask;
    if (ideal == -1) { if (!(mask >> (o->ideal_core & 63) & 1)) o->ideal_core = mask ? (int)__builtin_ctzll(mask) : OS_DEFAULT_CORE; }   /* IdealCoreDontCare */
    else o->ideal_core = ideal;
    apply_sched(h, self, o->core_mask);
}
static void os_GetThreadCoreMask(Ctx *c) {            /* (int *outIdealCore, Bit64 *outMask, const ThreadType*) */
    Obj *o = thread_obj(A2);
    if (A0) wr32(A0, (uint32_t)o->ideal_core);
    if (A1) wr64(A1, o->core_mask);
}
static void os_GetCurrentCoreNumber(Ctx *c) {
    if (console_cores()) {                                /* the core the thread is on now, as the console answers */
#ifdef _WIN32
        uintptr_t here = (uintptr_t)1 << GetCurrentProcessorNumber();
#else
        int cpu = sched_getcpu();
        uintptr_t here = cpu >= 0 ? (uintptr_t)1 << cpu : 0;
#endif
        for (int i = 0; i < 3; i++) if (g_cc_core[i] == here) { RET(i); return; }
    }
    RET((int64_t)thread_obj(t_thread)->ideal_core);
}
static void os_AvailableCoreMask(Ctx *c) { RET(7); }
static void nothing(Ctx *c) { (void)c; }
static void ret0(Ctx *c) { RET(0); }

/* thread-local storage slots */
static volatile int32_t g_tls_next;
/* (TlsSlot *out, void (*destructor)(uintptr_t value)) -> Result. The destructor of a slot runs when a thread ends
 * with a non-zero value in it; FreeTlsSlot makes the slot available again. */
static void os_AllocateTlsSlot(Ctx *c) {
    (void)g_tls_next;
    for (int i = 0; i < 64; i++)
        if (plat_atomic_cas(&g_tls_used[i], 1, 0) == 0) { g_tls_dtor[i] = A1; wr32(A0, (uint32_t)i); RET(0); return; }
    guest_trap("nn::os::AllocateTlsSlot: all 64 slots are in use (the console has 32)", c->x[30] - 4);
}
static void os_FreeTlsSlot(Ctx *c) { uint32_t i = (uint32_t)A0 & 63; g_tls_dtor[i] = 0; plat_atomic_xchg(&g_tls_used[i], 0); }
static void tls_run_destructors(Ctx *c) {
    for (int i = 0; i < 64; i++) {
        uint64_t v = t_tls[i], fn = g_tls_dtor[i];
        t_tls[i] = 0;
        if (v && fn && g_tls_used[i]) { c->x[0] = v; call_guest(c, fn); }
    }
}
static void os_GetTlsValue(Ctx *c) { RET(t_tls[A0 & 63]); }
static void os_SetTlsValue(Ctx *c) { t_tls[A0 & 63] = A1; }

/* ---------------------------------------------------------------- SMO_WAITMAP
 * Android port, a diagnostic (SMO_WAITMAP=1): every wait of a guest thread on an nn::os object that blocks (events,
 * system events, condition variables, semaphores, message queues, contended mutexes, WaitAny) is counted per (waiting
 * thread, object): how often, how long, and which thread signalled (or unlocked) the object last and how long after
 * that signal the waiter ran again. Every 10 s the 30 largest are printed as "nn_waitmap:" lines, per second. */
static int g_wm = -1;
typedef struct { uint64_t waiter, obj, by; int kind; int64_t n, ns, max_ns, lat_ns, lat_n; } WmEnt;
#define WM_N 2048
static WmEnt g_wm_t[WM_N];
static int g_wm_lock;
static int64_t g_wm_t0;
static int wm_on(void) { if (g_wm < 0) { const char *e = getenv("SMO_WAITMAP"); g_wm = e && atoi(e); } return g_wm; }
static int64_t wm_now(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec; }
static const char *wm_name(uint64_t key, char *buf, size_t n) {
    if (!key) return "host";
    Obj *o = obj_raw(key);
    snprintf(buf, n, "%s", o && o->name ? (const char *)(uintptr_t)o->name : "?");
    return buf;
}
static void wm_signal(Obj *o) { if (g_wm > 0) { o->sig_by = t_thread; o->sig_t = wm_now(); } }
static void wm_print(int64_t now) {                /* g_wm_lock held */
    static const char *const kind[] = {"event", "system event", "condition variable", "semaphore", "message queue (receive)", "message queue (send)", "mutex", "WaitAny"};
    double sec = (double)(now - g_wm_t0) / 1e9;
    int order[30], n = 0;
    for (int k = 0; k < 30; k++) {
        int best = -1;
        for (int i = 0; i < WM_N; i++) {
            if (!g_wm_t[i].n) continue;
            int used = 0;
            for (int j = 0; j < n; j++) used |= order[j] == i;
            if (!used && (best < 0 || g_wm_t[i].ns > g_wm_t[best].ns)) best = i;
        }
        if (best < 0) break;
        order[n++] = best;
    }
    printf("nn_waitmap: the largest blocking waits of the last %.1f s (per second: count, ms waited; the longest; the wake-up after the last signal)\n", sec);
    for (int k = 0; k < n; k++) {
        WmEnt *e = &g_wm_t[order[k]];
        char a[64], b[64];
        printf("nn_waitmap:   %-24s waits on %-24s %llx: %6.1f/s %7.1f ms/s, longest %6.1f ms, last signalled by %-24s wake-up %5.0f us\n",
               wm_name(e->waiter, a, sizeof a), kind[e->kind], (unsigned long long)e->obj, (double)e->n / sec, (double)e->ns / 1e6 / sec, (double)e->max_ns / 1e6,
               wm_name(e->by, b, sizeof b), e->lat_n ? (double)e->lat_ns / (double)e->lat_n / 1e3 : 0.0);
    }
    fflush(stdout);
    memset(g_wm_t, 0, sizeof g_wm_t);
    g_wm_t0 = now;
}
static void wm_note(int kind, Obj *o, int64_t t0) {
    int64_t t1 = wm_now(), ns = t1 - t0;
    if (ns < 20000) return;                        /* under 20 us: not a wait that blocked */
    while (__atomic_exchange_n(&g_wm_lock, 1, __ATOMIC_ACQUIRE)) {}
    if (!g_wm_t0) g_wm_t0 = t1;
    unsigned h = (unsigned)(((t_thread >> 4) ^ (o->key * 31)) * 2654435761u) % WM_N;
    for (int probe = 0; probe < 32; probe++, h = (h + 1) % WM_N) {
        WmEnt *e = &g_wm_t[h];
        if (e->n && (e->waiter != t_thread || e->obj != o->key || e->kind != kind)) continue;
        e->waiter = t_thread; e->obj = o->key; e->kind = kind; e->by = o->sig_by;
        e->n++; e->ns += ns; if (ns > e->max_ns) e->max_ns = ns;
        if (o->sig_t > t0) { e->lat_ns += t1 - o->sig_t; e->lat_n++; }
        break;
    }
    if (t1 - g_wm_t0 > 10000000000LL) wm_print(t1);
    __atomic_store_n(&g_wm_lock, 0, __ATOMIC_RELEASE);
}
#define WM_BEGIN int64_t wm_t0_ = wm_on() ? wm_now() : 0
#define WM_END(kind, o) do { if (wm_t0_) wm_note(kind, o, wm_t0_); } while (0)

/* ---------------------------------------------------------------- mutexes */
static void mtx_init(Ctx *c) { (void)obj(A0); }
static void mtx_lock(Ctx *c) {
    Obj *o = obj(A0);
    if (wm_on() && !plat_mutex_trylock(&o->cs)) { WM_BEGIN; plat_mutex_lock(&o->cs); WM_END(6, o); return; }
    if (!g_wm) plat_mutex_lock(&o->cs);
}
static void mtx_unlock(Ctx *c) { Obj *o = obj(A0); wm_signal(o); plat_mutex_unlock(&o->cs); }
static void mtx_trylock(Ctx *c) { RET(plat_mutex_trylock(&obj(A0)->cs) ? 1 : 0); }
/* hitch.c (SMO_HITCH): host thread id that owns the guest mutex `key` now, 0 = nobody. Diagnostics only. */
#ifdef _WIN32
uint32_t os_mutex_owner(uint64_t key) { return key ? (uint32_t)(uintptr_t)obj(key)->cs.OwningThread : 0; }
#else
uint32_t os_mutex_owner(uint64_t key) { (void)key; return 0; }   /* a pthread mutex does not tell its owner */
#endif
static void pmtx_lock(Ctx *c) { plat_mutex_lock(&obj(A0)->cs); RET(0); }
static void pmtx_unlock(Ctx *c) { plat_mutex_unlock(&obj(A0)->cs); RET(0); }
static void pmtx_trylock(Ctx *c) { RET(plat_mutex_trylock(&obj(A0)->cs) ? 0 : 16); }   /* EBUSY */

/* ---------------------------------------------------------------- events (Event, LightEvent, SystemEvent) */
/* nn::os::EventClearMode: 0 = ManualClear, 1 = AutoClear */
static void ev_init(Ctx *c) { obj_reset(A0); Obj *o = obj(A0); o->signaled = (int)(A1 & 1); o->autoclear = A2 == 1; }
static void ev_signal(Ctx *c) {
    Obj *o = obj(A0);
    plat_mutex_lock(&o->cs);
    wm_signal(o);
    o->signaled = 1;
    plat_cond_broadcast(&o->cv);
    plat_mutex_unlock(&o->cs);
}
/* Host-side access to guest events (used for the display's vsync event). */
void os_init_event(uint64_t key, int autoclear) { obj_reset(key); Obj *o = obj(key); o->signaled = 0; o->autoclear = autoclear; }
void os_signal_event(uint64_t key) {
    Obj *o = obj(key);
    plat_mutex_lock(&o->cs);
    wm_signal(o);
    o->signaled = 1;
    plat_cond_broadcast(&o->cv);
    plat_mutex_unlock(&o->cs);
}
void os_clear_event(uint64_t key) { Obj *o = obj(key); plat_mutex_lock(&o->cs); o->signaled = 0; plat_mutex_unlock(&o->cs); }
static void ev_clear(Ctx *c) { Obj *o = obj(A0); plat_mutex_lock(&o->cs); o->signaled = 0; plat_mutex_unlock(&o->cs); }
static int ev_wait_ms(Obj *o, uint32_t ms) {
    int ok = 1;
    if (ms) PLAT_SPIN_WHILE(!__atomic_load_n(&o->signaled, __ATOMIC_ACQUIRE));       /* plat.h: spinning before blocking */
    WM_BEGIN;
    plat_mutex_lock(&o->cs);
    while (!o->signaled) {
        if (!plat_cond_wait(&o->cv, &o->cs, ms)) { ok = o->signaled; break; }
    }
    if (ms) WM_END(0, o);
    if (ok && o->autoclear) o->signaled = 0;
    plat_mutex_unlock(&o->cs);
    return ok;
}
static void ev_wait(Ctx *c) { ev_wait_ms(obj(A0), PLAT_INFINITE); }
static void ev_trywait(Ctx *c) { RET(ev_wait_ms(obj(A0), 0)); }
static void ev_timedwait(Ctx *c) { RET(ev_wait_ms(obj(A0), ns_to_ms((int64_t)A1))); }

/* ---- SMO port: system events shared through kernel handles.
 * [doc] A SystemEvent is a kernel event: CreateSystemEvent makes it with a readable and a writable handle; another
 * SystemEventType (in this or another process) can be attached to those handles, and a signal through one is seen by
 * every waiter on the event. SMO's audio server (lifted SDK code) creates the renderer's event and the client attaches
 * the game's SystemEvent to its readable handle. Model: handles name host event objects (g_handles); an attached
 * SystemEventType resolves to the event it shares (Obj.alias, followed by obj()). Each SystemEventType keeps ITS OWN
 * clear mode for its waits (AutoClear = a successful wait clears the shared event), as on the console. */
#define MAX_HANDLES 1024
static Obj *g_handles[MAX_HANDLES];
static int32_t g_handle_next = 1;
static uint32_t handle_new(Obj *o) {
    int32_t i = plat_atomic_inc(&g_handle_next) - 1;
    if (i >= MAX_HANDLES) guest_trap("os: too many system event handles (os_threads.c MAX_HANDLES)", 0);
    g_handles[i] = o;
    return 0x50000u + (uint32_t)i;
}
static Obj *handle_obj(uint32_t h) { uint32_t i = h - 0x50000u; return i < MAX_HANDLES ? g_handles[i] : NULL; }
static int sev_wait(uint64_t key, uint32_t ms) {
    Obj *raw = obj_raw(key), *o = obj(key);
    int ok = 1;
    if (ms) PLAT_SPIN_WHILE(!__atomic_load_n(&o->signaled, __ATOMIC_ACQUIRE));
    WM_BEGIN;
    plat_mutex_lock(&o->cs);
    while (!o->signaled) {
        if (!plat_cond_wait(&o->cv, &o->cs, ms)) { ok = o->signaled; break; }
    }
    if (ms) WM_END(1, o);
    if (ok && raw->autoclear) o->signaled = 0;
    plat_mutex_unlock(&o->cs);
    return ok;
}
static void sev_create(Ctx *c) {               /* (SystemEventType*, EventClearMode, bool interprocess) -> Result */
    Obj *raw = obj_raw(A0);
    raw->alias = NULL;
    obj_reset(A0);
    raw->autoclear = A1 == 1;
    raw->rhandle = handle_new(raw);
    raw->whandle = handle_new(raw);
    RET(0);
}
static void sev_attach_readable(Ctx *c) {      /* (SystemEventType*, Handle, bool managed, EventClearMode) */
    Obj *raw = obj_raw(A0), *t = handle_obj((uint32_t)A1);
    if (!t) guest_trap("os: AttachReadableHandleToSystemEvent with a handle no event of this process has", c->x[30] - 4);
    raw->alias = t == raw ? NULL : t;
    raw->autoclear = A3 == 1;
}
static void sev_attach_writable(Ctx *c) {      /* (SystemEventType*, Handle, bool managed, EventClearMode) */
    Obj *raw = obj_raw(A0), *t = handle_obj((uint32_t)A1);
    if (!t) guest_trap("os: AttachWritableHandleToSystemEvent with a handle no event of this process has", c->x[30] - 4);
    raw->alias = t == raw ? NULL : t;
    raw->autoclear = A3 == 1;
}
static void sev_attach(Ctx *c) {               /* (SystemEventType*, rHandle, bool, wHandle, bool, EventClearMode) */
    Obj *raw = obj_raw(A0), *t = handle_obj((uint32_t)A1);
    if (!t) t = handle_obj((uint32_t)A3);
    if (!t) guest_trap("os: AttachSystemEvent with handles no event of this process has", c->x[30] - 4);
    raw->alias = t == raw ? NULL : t;
    raw->autoclear = c->x[5] == 1;
}
/* Host side: a new system event with a handle (as a system module would create one and pass its readable handle);
 * returns the handle, *key receives a host key to signal it with os_signal_event. */
uint32_t os_system_event_new(int autoclear, uint64_t *key) {
    uint64_t k = (uint64_t)(uintptr_t)calloc(1, 64);            /* a unique key no guest object has */
    obj_reset(k);
    Obj *o = obj_raw(k);
    o->autoclear = autoclear;
    o->rhandle = handle_new(o);
    o->whandle = handle_new(o);
    if (key) *key = k;
    return o->rhandle;
}
static void sev_get_readable(Ctx *c) { Obj *o = obj(A0); if (!o->rhandle) o->rhandle = handle_new(o); RET(o->rhandle); }
static void sev_get_writable(Ctx *c) { Obj *o = obj(A0); if (!o->whandle) o->whandle = handle_new(o); RET(o->whandle); }
static void sev_detach(Ctx *c) { obj_raw(A0)->alias = NULL; RET(0); }
static void sev_destroy(Ctx *c) { obj_raw(A0)->alias = NULL; }
static void sev_wait_h(Ctx *c) { sev_wait(A0, PLAT_INFINITE); }
static void sev_trywait_h(Ctx *c) { RET(sev_wait(A0, 0)); }
static void sev_timedwait_h(Ctx *c) { RET(sev_wait(A0, ns_to_ms((int64_t)A1))); }

/* ---------------------------------------------------------------- condition variables */
static void cv_wait(Ctx *c) { WM_BEGIN; plat_cond_wait(&obj(A0)->cv, &obj(A1)->cs, PLAT_INFINITE); WM_END(2, obj(A0)); }
static void cv_timedwait(Ctx *c) {                    /* returns ConditionVariableStatus: 0 timeout, 1 signalled */
    WM_BEGIN;
    RET(plat_cond_wait(&obj(A0)->cv, &obj(A1)->cs, ns_to_ms((int64_t)A2)) ? 1 : 0);
    WM_END(2, obj(A0));
}
static void cv_signal(Ctx *c) { wm_signal(obj(A0)); plat_cond_signal(&obj(A0)->cv); }
static void cv_broadcast(Ctx *c) { wm_signal(obj(A0)); plat_cond_broadcast(&obj(A0)->cv); }
/* std::condition_variable: the argument is a unique_lock<mutex> whose first word is the mutex pointer */
static void std_cv_wait(Ctx *c) { WM_BEGIN; plat_cond_wait(&obj(A0)->cv, &obj(rd64(A1))->cs, PLAT_INFINITE); WM_END(2, obj(A0)); }
static void std_cv_timedwait(Ctx *c) {                /* deadline: system_clock nanoseconds since 1970 */
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    int64_t now = (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
    plat_cond_wait(&obj(A0)->cv, &obj(rd64(A1))->cs, ns_to_ms((int64_t)A2 - now));
}

/* ---------------------------------------------------------------- semaphores */
static void sem_init(Ctx *c) { obj_reset(A0); Obj *o = obj(A0); o->count = (int)A1; o->max = (int)A2; }
static void sem_acquire(Ctx *c) {
    Obj *o = obj(A0);
    WM_BEGIN;
    plat_mutex_lock(&o->cs);
    while (o->count <= 0) plat_cond_wait(&o->cv, &o->cs, PLAT_INFINITE);
    WM_END(3, o);
    o->count--;
    plat_mutex_unlock(&o->cs);
}
static void sem_release(Ctx *c) {
    Obj *o = obj(A0);
    plat_mutex_lock(&o->cs);
    if (o->count < o->max) o->count++;               /* nn::os semaphores saturate at their maximum */
    wm_signal(o);
    plat_cond_signal(&o->cv);
    plat_mutex_unlock(&o->cs);
}

/* ---------------------------------------------------------------- message queues (of 64-bit values) */
static void mq_init(Ctx *c) {                         /* (queue, buffer, count): host-side storage is used */
    obj_reset(A0);
    Obj *o = obj(A0);
    free(o->buf);
    o->cap = A2 ? A2 : 1;
    o->buf = calloc((size_t)o->cap, 8);
}
static int mq_put(Obj *o, uint64_t v, int front, uint32_t ms) {
    int ok = 1;
    WM_BEGIN;
    plat_mutex_lock(&o->cs);
    while (o->len == o->cap) {
        if (!plat_cond_wait(&o->cv, &o->cs, ms)) { ok = o->len < o->cap; break; }
    }
    if (ms) WM_END(5, o);
    wm_signal(o);
    if (ok) {
        if (front) { o->head = (o->head + o->cap - 1) % o->cap; o->buf[o->head] = v; }
        else o->buf[(o->head + o->len) % o->cap] = v;
        o->len++;
        plat_cond_broadcast(&o->cv);
    }
    plat_mutex_unlock(&o->cs);
    return ok;
}
static int mq_get(Obj *o, uint64_t out, uint32_t ms) {
    int ok = 1;
    WM_BEGIN;
    plat_mutex_lock(&o->cs);
    while (o->len == 0) {
        if (!plat_cond_wait(&o->cv, &o->cs, ms)) { ok = o->len > 0; break; }
    }
    if (ms) WM_END(4, o);
    wm_signal(o);
    if (ok) {
        wr64(out, o->buf[o->head]);
        o->head = (o->head + 1) % o->cap;
        o->len--;
        plat_cond_broadcast(&o->cv);
    }
    plat_mutex_unlock(&o->cs);
    return ok;
}
static void mq_send(Ctx *c) { mq_put(obj(A0), A1, 0, PLAT_INFINITE); }
static void mq_trysend(Ctx *c) { RET(mq_put(obj(A0), A1, 0, 0)); }
static void mq_jam(Ctx *c) { mq_put(obj(A0), A1, 1, PLAT_INFINITE); }
static void mq_tryjam(Ctx *c) { RET(mq_put(obj(A0), A1, 1, 0)); }
static void mq_receive(Ctx *c) { mq_get(obj(A1), A0, PLAT_INFINITE); }
static void mq_tryreceive(Ctx *c) { RET(mq_get(obj(A1), A0, 0)); }
static void mq_timedreceive(Ctx *c) { RET(mq_get(obj(A1), A0, ns_to_ms((int64_t)A2))); }
/* SMO port: PeekMessageQueue / TryPeekMessageQueue (out, queue): the oldest message without taking it (sead::MessageQueue::peek). */
static int mq_peek(Obj *o, uint64_t out, uint32_t ms) {
    int ok = 1;
    plat_mutex_lock(&o->cs);
    while (o->len == 0) {
        if (!plat_cond_wait(&o->cv, &o->cs, ms)) { ok = o->len > 0; break; }
    }
    if (ok) wr64(out, o->buf[o->head]);
    plat_mutex_unlock(&o->cs);
    return ok;
}
static void mq_peek_h(Ctx *c) { mq_peek(obj(A1), A0, PLAT_INFINITE); }
static void mq_trypeek(Ctx *c) { RET(mq_peek(obj(A1), A0, 0)); }

/* ---------------------------------------------------------------- multi-wait (polling implementation) */
static void mw_init(Ctx *c) { obj_reset(A0); }
static void mw_holder_init(Ctx *c) { Obj *h = obj(A0); h->target = A1; h->user = 0; }
static void mw_link(Ctx *c) { Obj *m = obj(A0); if (m->holder_count < 64) m->holders[m->holder_count++] = obj(A1); }
static void mw_unlink(Ctx *c) {
    Obj *h = obj(A0);
    plat_rw_wrlock(&g_table_lock);
    for (int b = 0; b < BUCKETS; b++)
        for (Obj *m = g_table[b]; m; m = m->next)
            for (int i = 0; i < m->holder_count; i++)
                if (m->holders[i] == h) { m->holders[i] = m->holders[--m->holder_count]; i--; }
    plat_rw_wrunlock(&g_table_lock);
}
static void mw_set_user(Ctx *c) { obj(A0)->user = A1; }
static void mw_get_user(Ctx *c) { RET(obj(A0)->user); }
static uint64_t mw_poll(Obj *m) {
    for (int i = 0; i < m->holder_count; i++) {
        Obj *t = obj(m->holders[i]->target);
        if (t->signaled || t->finished) return m->holders[i]->key;
    }
    return 0;
}
static void mw_waitany(Ctx *c) { Obj *m = obj(A0); uint64_t r; WM_BEGIN; while (!(r = mw_poll(m))) plat_sleep_ms(1); WM_END(7, m); RET(r); }
static void mw_trywaitany(Ctx *c) { RET(mw_poll(obj(A0))); }

/* ---------------------------------------------------------------- fibers */
#ifdef _WIN32
static void CALLBACK fiber_main(void *arg) {
    Obj *o = arg;
    Ctx *c = new_ctx(o->stack + o->stack_size);
    plat_vm_free((void *)(uintptr_t)c->tpidr, 0);   /* a fiber has no thread block of its own: it uses the one of the thread running it */
    for (;;) {
        c->tpidr = o->tp;
        c->x[0] = o->arg;
        hitch_cur_ctx(c);
        call_guest(c, o->fn);                         /* returns the fiber to switch to next, or null */
        uint64_t next = c->x[0];
        if (next) obj(next)->tp = c->tpidr;
        t_fiber = next;
        SwitchToFiber(next ? obj(next)->fiber : t_thread_fiber);
    }
}
static void fib_init(Ctx *c) {                        /* (FiberType*, fn, arg, stack, stackSize, flags) */
    Obj *o = obj(A0);
    o->fn = A1; o->arg = A2; o->stack = A3; o->stack_size = A4;
    o->fiber = CreateFiberEx(1 << 20, (size_t)256 << 20, 0, fiber_main, o);
    RET(0);
}
static void fib_switch(Ctx *c) {
    if (!t_thread_fiber) t_thread_fiber = ConvertThreadToFiber(NULL);
    uint64_t next = A0, self = t_fiber;
    if (next) obj(next)->tp = c->tpidr;               /* the thread block (errno, ...) belongs to the host thread, not to the fiber */
    t_fiber = next;
    SwitchToFiber(next ? obj(next)->fiber : t_thread_fiber);
    if (self) c->tpidr = obj(self)->tp;               /* resumed, possibly by another thread */
    hitch_cur_ctx(c);
}
#else
/* Android port: the game never makes a fiber (it imports only GetCurrentFiber); a call means a game this runtime was
 * not checked with. */
static void fib_init(Ctx *c) { guest_trap("nn::os::InitializeFiber: fibers are not implemented on this host", c->x[30] - 4); }
static void fib_switch(Ctx *c) { guest_trap("nn::os::SwitchToFiber: fibers are not implemented on this host", c->x[30] - 4); }
#endif
static void fib_current(Ctx *c) { RET(t_fiber); }
#ifdef _WIN32
static void fib_finalize(Ctx *c) { Obj *o = obj(A0); if (o->fiber) { DeleteFiber(o->fiber); o->fiber = NULL; } }
#else
static void fib_finalize(Ctx *c) { (void)c; }
#endif

/* ---------------------------------------------------------------- misc */
static void os_ConvertToTick(Ctx *c) { RET((uint64_t)((double)(int64_t)A0 * (double)TICK_HZ / 1e9)); }
static void os_ConvertToTimeSpan(Ctx *c) { RET((uint64_t)((double)(int64_t)A0 * 1e9 / (double)TICK_HZ)); }
/* The console fills the buffer from a generator seeded with kernel entropy: unpredictable bytes. Same here, from the
 * system generator (RtlGenRandom). */
#ifdef _WIN32
BOOLEAN NTAPI SystemFunction036(void * buffer, uint32_t length);
#else
#include <sys/random.h>
static int SystemFunction036(void *buffer, uint32_t length) {     /* the kernel's generator, as RtlGenRandom is */
    for (uint32_t done = 0; done < length;) {
        ssize_t n = getrandom((uint8_t *)buffer + done, length - done, 0);
        if (n <= 0) return 0;
        done += (uint32_t)n;
    }
    return 1;
}
#endif
/* Test switch SMO_FIXED_SEED=<n> (track V, session 4): the bytes come from a splitmix64 stream seeded with n instead, so
 * that a scripted match meets the same random stage / CPU fighter in every run. Each call is logged (first 16). */
int host_fixed_random(void *buffer, uint64_t size, const char *who, uint64_t lr) {
    static int mode = -1;
    static volatile int32_t lock, calls;
    static uint64_t state;
    if (mode < 0) { const char *e = getenv("SMO_FIXED_SEED"); if (e && *e) { state = strtoull(e, NULL, 0) * 0x9E3779B97F4A7C15ull + 1; mode = 1; } else mode = 0; }
    if (!mode) return 0;
    while (plat_atomic_xchg(&lock, 1)) plat_cpu_relax();
    for (uint64_t i = 0; i < size; i += 8) {
        uint64_t z = (state += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull; z = (z ^ (z >> 27)) * 0x94D049BB133111EBull; z ^= z >> 31;
        memcpy((uint8_t *)buffer + i, &z, size - i < 8 ? (size_t)(size - i) : 8);
    }
    int32_t n = ++calls;
    plat_atomic_xchg(&lock, 0);
    if (n <= 16) printf("fixed seed: %s call %ld, %llu bytes, caller %llx\n", who, n, (unsigned long long)size, (unsigned long long)lr);
    return 1;
}
static void os_GenerateRandomBytes(Ctx *c) {
    uint8_t *p = (uint8_t *)(uintptr_t)A0;
    if (host_fixed_random(p, A1, "nn::os::GenerateRandomBytes", c->x[30] - 4)) return;
    for (uint64_t done = 0; done < A1;) {
        uint32_t n = A1 - done > 0x10000000 ? 0x10000000 : (uint32_t)(A1 - done);
        if (!SystemFunction036(p + done, n)) guest_trap("nn::os::GenerateRandomBytes: the system random generator failed", c->x[30] - 4);
        done += n;
    }
}

#define OS(n) "_ZN2nn2os" n
static const ImportDef thread_imports[] = {
    {OS("12CreateThreadEPNS0_10ThreadTypeEPFvPvES3_S3_mi"), os_CreateThread},
    {OS("12CreateThreadEPNS0_10ThreadTypeEPFvPvES3_S3_mii"), os_CreateThreadOnCore},
    {OS("25GetCurrentProcessorNumberEv"), os_GetCurrentCoreNumber}, {OS("20GetCurrentCoreNumberEv"), os_GetCurrentCoreNumber},
    {OS("11StartThreadEPNS0_10ThreadTypeE"), os_StartThread}, {OS("10WaitThreadEPNS0_10ThreadTypeE"), os_WaitThread},
    {OS("13DestroyThreadEPNS0_10ThreadTypeE"), os_DestroyThread}, {OS("13SetThreadNameEPNS0_10ThreadTypeEPKc"), os_SetThreadName},
    {OS("17SetThreadCoreMaskEPNS0_10ThreadTypeEim"), os_SetThreadCoreMask}, {OS("17GetThreadCoreMaskEPiPmPKNS0_10ThreadTypeE"), os_GetThreadCoreMask},
    {OS("26GetThreadAvailableCoreMaskEv"), os_AvailableCoreMask},
    {OS("16GetCurrentThreadEv"), os_GetCurrentThread}, {OS("17GetThreadPriorityEPKNS0_10ThreadTypeE"), os_GetThreadPriority},
    {OS("24GetThreadCurrentPriorityEPKNS0_10ThreadTypeE"), os_GetThreadPriority},
    {OS("20ChangeThreadPriorityEPNS0_10ThreadTypeEi"), os_ChangeThreadPriority}, {OS("11GetThreadIdEPKNS0_10ThreadTypeE"), os_GetThreadId},
    {OS("11YieldThreadEv"), os_YieldThread}, {OS("11SleepThreadENS_8TimeSpanE"), os_SleepThread},
    {OS("19GetCurrentStackInfoEPmS1_"), os_GetCurrentStackInfo},
    {OS("15AllocateTlsSlotEPNS0_7TlsSlotEPFvmE"), os_AllocateTlsSlot}, {OS("18SdkAllocateTlsSlotEPNS0_7TlsSlotEPFvmE"), os_AllocateTlsSlot},
    {OS("11FreeTlsSlotENS0_7TlsSlotE"), os_FreeTlsSlot}, {OS("11GetTlsValueENS0_7TlsSlotE"), os_GetTlsValue}, {OS("11SetTlsValueENS0_7TlsSlotEm"), os_SetTlsValue},
    {OS("15InitializeMutexEPNS0_9MutexTypeEbi"), mtx_init}, {OS("13FinalizeMutexEPNS0_9MutexTypeE"), nothing},
    {OS("9LockMutexEPNS0_9MutexTypeE"), mtx_lock}, {OS("11UnlockMutexEPNS0_9MutexTypeE"), mtx_unlock}, {OS("12TryLockMutexEPNS0_9MutexTypeE"), mtx_trylock},
    {"pthread_mutex_init", ret0}, {"pthread_mutex_destroy", ret0}, {"pthread_mutex_lock", pmtx_lock}, {"pthread_mutex_unlock", pmtx_unlock},
    {"pthread_mutex_trylock", pmtx_trylock},
    {"_ZNSt3__15mutex4lockEv", mtx_lock}, {"_ZNSt3__15mutex6unlockEv", mtx_unlock}, {"_ZNSt3__15mutex8try_lockEv", mtx_trylock},
    {"_ZNSt3__115recursive_mutex4lockEv", mtx_lock}, {"_ZNSt3__115recursive_mutex6unlockEv", mtx_unlock},
    {"_ZNSt3__115recursive_mutex8try_lockEv", mtx_trylock},
    {OS("15InitializeEventEPNS0_9EventTypeEbNS0_14EventClearModeE"), ev_init}, {OS("13FinalizeEventEPNS0_9EventTypeE"), nothing},
    {OS("11SignalEventEPNS0_9EventTypeE"), ev_signal}, {OS("10ClearEventEPNS0_9EventTypeE"), ev_clear}, {OS("9WaitEventEPNS0_9EventTypeE"), ev_wait},
    {OS("14TimedWaitEventEPNS0_9EventTypeENS_8TimeSpanE"), ev_timedwait},
    {OS("20InitializeLightEventEPNS0_14LightEventTypeEbNS0_14EventClearModeE"), ev_init}, {OS("18FinalizeLightEventEPNS0_14LightEventTypeE"), nothing},
    {OS("16SignalLightEventEPNS0_14LightEventTypeE"), ev_signal}, {OS("15ClearLightEventEPNS0_14LightEventTypeE"), ev_clear},
    {OS("14WaitLightEventEPNS0_14LightEventTypeE"), ev_wait},
    {OS("18DestroySystemEventEPNS0_15SystemEventTypeE"), sev_destroy}, {OS("16ClearSystemEventEPNS0_15SystemEventTypeE"), ev_clear},
    {OS("18TryWaitSystemEventEPNS0_15SystemEventTypeE"), sev_trywait_h}, {OS("15WaitSystemEventEPNS0_15SystemEventTypeE"), sev_wait_h},
    {OS("20TimedWaitSystemEventEPNS0_15SystemEventTypeENS_8TimeSpanE"), sev_timedwait_h},
    {OS("27InitializeConditionVariableEPNS0_21ConditionVariableTypeE"), mtx_init}, {OS("25FinalizeConditionVariableEPNS0_21ConditionVariableTypeE"), nothing},
    {OS("21WaitConditionVariableEPNS0_21ConditionVariableTypeEPNS0_9MutexTypeE"), cv_wait},
    {OS("26TimedWaitConditionVariableEPNS0_21ConditionVariableTypeEPNS0_9MutexTypeENS_8TimeSpanE"), cv_timedwait},
    {OS("23SignalConditionVariableEPNS0_21ConditionVariableTypeE"), cv_signal}, {OS("26BroadcastConditionVariableEPNS0_21ConditionVariableTypeE"), cv_broadcast},
    {"_ZNSt3__118condition_variable4waitERNS_11unique_lockINS_5mutexEEE", std_cv_wait},
    {"_ZNSt3__118condition_variable15__do_timed_waitERNS_11unique_lockINS_5mutexEEENS_6chrono10time_pointINS5_12system_clockENS5_8durationIxNS_5ratioILl1ELl1000000000EEEEEEE", std_cv_timedwait},
    {"_ZNSt3__118condition_variable10notify_oneEv", cv_signal}, {"_ZNSt3__118condition_variable10notify_allEv", cv_broadcast},
    {"pthread_cond_broadcast", cv_broadcast}, {"pthread_cond_destroy", ret0},
    {OS("19InitializeSemaphoreEPNS0_13SemaphoreTypeEii"), sem_init}, {OS("17FinalizeSemaphoreEPNS0_13SemaphoreTypeE"), nothing},
    {OS("16AcquireSemaphoreEPNS0_13SemaphoreTypeE"), sem_acquire}, {OS("16ReleaseSemaphoreEPNS0_13SemaphoreTypeE"), sem_release},
    {OS("22InitializeMessageQueueEPNS0_16MessageQueueTypeEPmm"), mq_init}, {OS("20FinalizeMessageQueueEPNS0_16MessageQueueTypeE"), nothing},
    {OS("16SendMessageQueueEPNS0_16MessageQueueTypeEm"), mq_send}, {OS("19TrySendMessageQueueEPNS0_16MessageQueueTypeEm"), mq_trysend},
    {OS("15JamMessageQueueEPNS0_16MessageQueueTypeEm"), mq_jam}, {OS("18TryJamMessageQueueEPNS0_16MessageQueueTypeEm"), mq_tryjam},
    {OS("19ReceiveMessageQueueEPmPNS0_16MessageQueueTypeE"), mq_receive}, {OS("22TryReceiveMessageQueueEPmPNS0_16MessageQueueTypeE"), mq_tryreceive},
    {OS("24TimedReceiveMessageQueueEPmPNS0_16MessageQueueTypeENS_8TimeSpanE"), mq_timedreceive},
    {OS("19InitializeMultiWaitEPNS0_13MultiWaitTypeE"), mw_init}, {OS("17FinalizeMultiWaitEPNS0_13MultiWaitTypeE"), nothing},
    {OS("25InitializeMultiWaitHolderEPNS0_19MultiWaitHolderTypeEPNS0_10ThreadTypeE"), mw_holder_init},
    {OS("25InitializeMultiWaitHolderEPNS0_19MultiWaitHolderTypeEPNS0_9EventTypeE"), mw_holder_init},
    {OS("23FinalizeMultiWaitHolderEPNS0_19MultiWaitHolderTypeE"), nothing},
    {OS("19LinkMultiWaitHolderEPNS0_13MultiWaitTypeEPNS0_19MultiWaitHolderTypeE"), mw_link},
    {OS("21UnlinkMultiWaitHolderEPNS0_19MultiWaitHolderTypeE"), mw_unlink},
    {OS("26SetMultiWaitHolderUserDataEPNS0_19MultiWaitHolderTypeEm"), mw_set_user},
    {OS("26GetMultiWaitHolderUserDataEPKNS0_19MultiWaitHolderTypeE"), mw_get_user},
    {OS("7WaitAnyEPNS0_13MultiWaitTypeE"), mw_waitany}, {OS("10TryWaitAnyEPNS0_13MultiWaitTypeE"), mw_trywaitany},
    {OS("15InitializeFiberEPNS0_9FiberTypeEPFS2_PvES3_S3_mi"), fib_init}, {OS("13FinalizeFiberEPNS0_9FiberTypeE"), fib_finalize},
    {OS("13SwitchToFiberEPNS0_9FiberTypeE"), fib_switch}, {OS("15GetCurrentFiberEv"), fib_current},
    {OS("13ConvertToTickENS_8TimeSpanE"), os_ConvertToTick}, {OS("17ConvertToTimeSpanENS0_4TickE"), os_ConvertToTimeSpan},
    {OS("19GenerateRandomBytesEPvm"), os_GenerateRandomBytes},
};

/* SMO port: SDK 3.5.1 forms the game imports. SetThreadNamePointer(ThreadType*, const char*): [doc] the thread's name is
 * the given string (kept by pointer); recorded for the thread reports. */
static void os_SetThreadNamePointer(Ctx *c) { obj(A0)->name = A1; }
/* GetThreadNamePointer(thread): the name set for it; a thread never named gets "Thread_0x<address>" as nn::os names it
 * (kept, so the pointer stays valid). Read by sead::nnsdk::PrintThreadInformation. */
static void os_GetThreadNamePointer(Ctx *c) {
    Obj *o = obj(A0);
    if (!o->name) { char *s = malloc(32); if (s) { snprintf(s, 32, "Thread_0x%llx", (unsigned long long)A0); o->name = (uint64_t)(uintptr_t)s; } }
    RET(o->name);
}
static const ImportDef smo_thread_imports[] = {
    {OS("19TimedWaitLightEventEPNS0_14LightEventTypeENS_8TimeSpanE"), ev_timedwait},
    {OS("20SetThreadNamePointerEPNS0_10ThreadTypeEPKc"), os_SetThreadNamePointer},
    {"nnosSetThreadNamePointer", os_SetThreadNamePointer},
    /* SMO port: imported by the game and without a handler before. TryWaitEvent: the movie player (MovieDecoderPlayer::update
     * polls its decoder's buffer events with it) and the sound library's loaders (nn::atk ...Loader::IsInUse, Finalize). */
    {OS("12TryWaitEventEPNS0_9EventTypeE"), ev_trywait},
    {OS("16PeekMessageQueueEPmPKNS0_16MessageQueueTypeE"), mq_peek_h},
    {OS("19TryPeekMessageQueueEPmPKNS0_16MessageQueueTypeE"), mq_trypeek},
    {OS("20GetThreadNamePointerEPKNS0_10ThreadTypeE"), os_GetThreadNamePointer},
};
/* SMO port: the system-event entry points, also for lifted SDK callers (sdklift_rules.txt cuts them) */
static const ImportDef sev_all[] = {
    {OS("17CreateSystemEventEPNS0_15SystemEventTypeENS0_14EventClearModeEb"), sev_create},
    {OS("17AttachSystemEventEPNS0_15SystemEventTypeEjbjbNS0_14EventClearModeE"), sev_attach},
    {OS("33AttachReadableHandleToSystemEventEPNS0_15SystemEventTypeEjbNS0_14EventClearModeE"), sev_attach_readable},
    {OS("33AttachWritableHandleToSystemEventEPNS0_15SystemEventTypeEjbNS0_14EventClearModeE"), sev_attach_writable},
    {OS("33DetachReadableHandleOfSystemEventEPNS0_15SystemEventTypeE"), sev_detach},
    {OS("33DetachWritableHandleOfSystemEventEPNS0_15SystemEventTypeE"), sev_detach},
    {OS("18DestroySystemEventEPNS0_15SystemEventTypeE"), sev_destroy},
    {OS("15WaitSystemEventEPNS0_15SystemEventTypeE"), sev_wait_h},
    {OS("18TryWaitSystemEventEPNS0_15SystemEventTypeE"), sev_trywait_h},
    {OS("20TimedWaitSystemEventEPNS0_15SystemEventTypeENS_8TimeSpanE"), sev_timedwait_h},
    {OS("17SignalSystemEventEPNS0_15SystemEventTypeE"), ev_signal},
    {OS("16ClearSystemEventEPNS0_15SystemEventTypeE"), ev_clear},
    {OS("30GetReadableHandleOfSystemEventEPKNS0_15SystemEventTypeE"), sev_get_readable},
    {OS("30GetWritableHandleOfSystemEventEPKNS0_15SystemEventTypeE"), sev_get_writable},
    {OS("30GetReadableHandleOfSystemEventEPNS0_15SystemEventTypeE"), sev_get_readable},
    {OS("30GetWritableHandleOfSystemEventEPNS0_15SystemEventTypeE"), sev_get_writable},
};
void sdk_host_override(const char *name, void (*fn)(Ctx *));   /* sdklift_rt.c */
void register_thread_imports(void) {
    register_imports(thread_imports, (int)(sizeof thread_imports / sizeof thread_imports[0]));
    register_imports(sev_all, (int)(sizeof sev_all / sizeof sev_all[0]));
    register_imports(smo_thread_imports, (int)(sizeof smo_thread_imports / sizeof smo_thread_imports[0]));
    for (int i = 0; i < (int)(sizeof sev_all / sizeof sev_all[0]); i++) sdk_host_override(sev_all[i].name, sev_all[i].fn);
}
int guest_thread_count(void) { return (int)g_thread_count; }
