/* Host primitives the runtime uses, on Win32 or on POSIX (Android, Linux).
 *
 * Android port (2026-10-27): the runtime was written against Win32. Its files now call these neutral names; on Windows
 * they are the Win32 objects they replaced (same behaviour as before), elsewhere pthreads, clock_gettime, mmap.
 * Locks are recursive (as a plat_mutex is). plat_cond_wait returns 1 when woken, 0 on time-out
 * (plat_cond_wait's convention). Atomics are the compiler's __atomic builtins on every host and work on any
 * integer width; the *_fetch / fetch_* names say whether the old or the new value is returned. */
#ifndef PLAT_H
#define PLAT_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

#define PLAT_INFINITE 0xFFFFFFFFu
#ifdef _WIN32
#define PLAT_SEP_S "\\"
#else
#define PLAT_SEP_S "/"
#endif

/* ---------------------------------------------------------------- atomics (all hosts) */
#define PLAT_SEQ __ATOMIC_SEQ_CST
#define plat_atomic_inc(p) __atomic_add_fetch((p), 1, PLAT_SEQ)                /* returns the new value */
#define plat_atomic_dec(p) __atomic_sub_fetch((p), 1, PLAT_SEQ)                /* returns the new value */
#define plat_atomic_add(p, v) __atomic_add_fetch((p), (v), PLAT_SEQ)           /* returns the new value */
#define plat_atomic_fetch_add(p, v) __atomic_fetch_add((p), (v), PLAT_SEQ)     /* returns the old value */
#define plat_atomic_xchg(p, v) __atomic_exchange_n((p), (v), PLAT_SEQ)         /* returns the old value */
#define plat_atomic_fetch_or(p, v) __atomic_fetch_or((p), (v), PLAT_SEQ)       /* returns the old value */
#define plat_atomic_load(p) __atomic_load_n((p), PLAT_SEQ)
#define plat_atomic_store(p, v) __atomic_store_n((p), (v), PLAT_SEQ)
/* compare-and-swap: stores newv when *p == cmp; returns the value *p had (plat_atomic_cas's convention) */
#define plat_atomic_cas(p, newv, cmp) __extension__({ __typeof__(*(p)) plat_e_ = (cmp); \
    __atomic_compare_exchange_n((p), &plat_e_, (newv), 0, PLAT_SEQ, PLAT_SEQ); plat_e_; })

#ifdef _WIN32
/* ================================================================ Win32 */
#include <windows.h>
#define PLAT_MAX_PATH MAX_PATH
#define PLAT_EXPORT __declspec(dllexport)
#define PLAT_IMPORT __declspec(dllimport)
typedef LARGE_INTEGER plat_li;
typedef CRITICAL_SECTION plat_mutex;
typedef CONDITION_VARIABLE plat_cond;
typedef SRWLOCK plat_rwlock;
#define PLAT_RWLOCK_INIT SRWLOCK_INIT
#define PLAT_COND_INIT CONDITION_VARIABLE_INIT
static inline void plat_mutex_init(plat_mutex *m) { InitializeCriticalSection(m); }
static inline int plat_mutex_init_spin(plat_mutex *m, uint32_t spin) { return InitializeCriticalSectionAndSpinCount(m, spin) != 0; }
static inline void plat_mutex_destroy(plat_mutex *m) { DeleteCriticalSection(m); }
static inline void plat_mutex_lock(plat_mutex *m) { EnterCriticalSection(m); }
static inline void plat_mutex_unlock(plat_mutex *m) { LeaveCriticalSection(m); }
static inline int plat_mutex_trylock(plat_mutex *m) { return TryEnterCriticalSection(m) != 0; }
static inline void plat_cond_init(plat_cond *c) { InitializeConditionVariable(c); }
static inline int plat_cond_wait(plat_cond *c, plat_mutex *m, uint32_t ms) { return SleepConditionVariableCS(c, m, ms) != 0; }
static inline void plat_cond_signal(plat_cond *c) { WakeConditionVariable(c); }
static inline void plat_cond_broadcast(plat_cond *c) { WakeAllConditionVariable(c); }
static inline void plat_rw_init(plat_rwlock *l) { InitializeSRWLock(l); }
static inline void plat_rw_rdlock(plat_rwlock *l) { AcquireSRWLockShared(l); }
static inline void plat_rw_rdunlock(plat_rwlock *l) { ReleaseSRWLockShared(l); }
static inline void plat_rw_wrlock(plat_rwlock *l) { AcquireSRWLockExclusive(l); }
static inline void plat_rw_wrunlock(plat_rwlock *l) { ReleaseSRWLockExclusive(l); }
static inline int64_t plat_qpc(void) { LARGE_INTEGER q; QueryPerformanceCounter(&q); return q.QuadPart; }
static inline int64_t plat_qpf(void) { LARGE_INTEGER q; QueryPerformanceFrequency(&q); return q.QuadPart; }
static inline uint64_t plat_ms64(void) { return GetTickCount64(); }
static inline void plat_sleep_ms(uint32_t ms) { Sleep(ms); }
static inline void plat_yield(void) { SwitchToThread(); }
static inline uint32_t plat_tid(void) { return GetCurrentThreadId(); }
static inline uint32_t plat_pid(void) { return GetCurrentProcessId(); }
static inline void plat_exit(int code) { ExitProcess((UINT)code); }
#define plat_strlwr _strlwr
#define plat_stricmp _stricmp
#define plat_strnicmp _strnicmp

#else
/* ================================================================ POSIX (Android, Linux) */
#include <pthread.h>
#include <sched.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <ctype.h>
#include <limits.h>
#include <sys/syscall.h>
#define PLAT_MAX_PATH 1024
#define PLAT_EXPORT __attribute__((visibility("default")))
#define PLAT_IMPORT
typedef union { int64_t QuadPart; struct { uint32_t LowPart; int32_t HighPart; }; } plat_li;   /* a 64-bit count */
typedef pthread_mutex_t plat_mutex;
typedef pthread_cond_t plat_cond;
typedef pthread_rwlock_t plat_rwlock;
#define PLAT_RWLOCK_INIT PTHREAD_RWLOCK_INITIALIZER
#define PLAT_COND_INIT PTHREAD_COND_INITIALIZER
static inline void plat_mutex_init(plat_mutex *m) {
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(m, &a);
    pthread_mutexattr_destroy(&a);
}
static inline int plat_mutex_init_spin(plat_mutex *m, uint32_t spin) { (void)spin; plat_mutex_init(m); return 1; }
static inline void plat_mutex_destroy(plat_mutex *m) { pthread_mutex_destroy(m); }
static inline void plat_mutex_lock(plat_mutex *m) { pthread_mutex_lock(m); }
static inline void plat_mutex_unlock(plat_mutex *m) { pthread_mutex_unlock(m); }
static inline int plat_mutex_trylock(plat_mutex *m) { return pthread_mutex_trylock(m) == 0; }
static inline void plat_cond_init(plat_cond *c) { pthread_cond_init(c, NULL); }
static inline int64_t plat_now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (int64_t)t.tv_sec * 1000000000 + t.tv_nsec; }
/* Waits on CLOCK_MONOTONIC (pthread_cond_clockwait: Android API 30, glibc 2.30), so a change of the wall clock cannot
 * lengthen or shorten the wait. */
static inline int plat_cond_wait(plat_cond *c, plat_mutex *m, uint32_t ms) {
    if (ms == PLAT_INFINITE) { pthread_cond_wait(c, m); return 1; }
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    t.tv_sec += ms / 1000;
    t.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (t.tv_nsec >= 1000000000L) { t.tv_sec++; t.tv_nsec -= 1000000000L; }
    return pthread_cond_clockwait(c, m, CLOCK_MONOTONIC, &t) == 0;
}
static inline void plat_cond_signal(plat_cond *c) { pthread_cond_signal(c); }
static inline void plat_cond_broadcast(plat_cond *c) { pthread_cond_broadcast(c); }
static inline void plat_rw_init(plat_rwlock *l) { pthread_rwlock_init(l, NULL); }
static inline void plat_rw_rdlock(plat_rwlock *l) { pthread_rwlock_rdlock(l); }
static inline void plat_rw_rdunlock(plat_rwlock *l) { pthread_rwlock_unlock(l); }
static inline void plat_rw_wrlock(plat_rwlock *l) { pthread_rwlock_wrlock(l); }
static inline void plat_rw_wrunlock(plat_rwlock *l) { pthread_rwlock_unlock(l); }
static inline int64_t plat_qpc(void) { return plat_now_ns(); }
static inline int64_t plat_qpf(void) { return 1000000000; }
static inline uint64_t plat_ms64(void) { return (uint64_t)(plat_now_ns() / 1000000); }
static inline void plat_sleep_ms(uint32_t ms) {
    if (!ms) { sched_yield(); return; }
    struct timespec t = {(time_t)(ms / 1000), (long)(ms % 1000) * 1000000L};
    while (nanosleep(&t, &t) != 0) {}
}
static inline void plat_yield(void) { sched_yield(); }
static inline uint32_t plat_tid(void) { return (uint32_t)gettid(); }
static inline uint32_t plat_pid(void) { return (uint32_t)getpid(); }
void plat_exit(int code) __attribute__((noreturn));
static inline char *plat_strlwr(char *s) { for (char *p = s; *p; p++) *p = (char)tolower((unsigned char)*p); return s; }
#define plat_stricmp strcasecmp
#define plat_strnicmp strncasecmp
#endif

/* ---------------------------------------------------------------- wall clock: nanoseconds since 1970 (all hosts) */
#include <time.h>
static inline int64_t plat_unix_ns(void) { struct timespec t; timespec_get(&t, TIME_UTC); return (int64_t)t.tv_sec * 1000000000 + t.tv_nsec; }

/* ---------------------------------------------------------------- sleeping with a nanosecond request (all hosts) */
void plat_sleep_ns(int64_t ns);

/* a spin-wait hint (YieldProcessor) */
#if defined(__aarch64__)
#define plat_cpu_relax() __asm__ __volatile__("yield" ::: "memory")
#elif defined(__x86_64__) || defined(_M_X64)
#define plat_cpu_relax() __builtin_ia32_pause()
#else
#define plat_cpu_relax() ((void)0)
#endif

/* Spinning before blocking (Android port, 2026-11-03). A thread that sleeps while it waits for another looks idle to a
 * phone's CPU governor, which then lowers the clocks of every step of the frame (PROGRESS.md, frame rate
 * investigation: 40 fps at the governor's clocks, 60 at high clocks). The waits on the frame's path first watch their
 * condition for a while, then block as before: SMO_SPINHOT_US microseconds on the threads that set the frame rate
 * (the render thread and the game thread that presents, plat_spin_mark_hot), SMO_SPINWAIT_US on every other thread.
 * Spinning on all ~19 game threads crowds out the work itself (measured: 20 ms everywhere halved the menu's rate);
 * the two hot threads can spin most of a frame on an 8-core phone. 0: no spinning. */
#include <stdlib.h>
#ifndef PLAT_SPIN_DEFAULT_US
#define PLAT_SPIN_DEFAULT_US 0
#endif
#ifndef PLAT_SPINHOT_DEFAULT_US
#define PLAT_SPINHOT_DEFAULT_US 0
#endif
#ifndef _WIN32
extern _Thread_local int plat_spin_hot_;              /* plat_posix.c */
extern int64_t plat_spinhot_ns_;                      /* plat_posix.c: the hot threads' spin, changed at run time (android_perf.c) */
static inline void plat_spin_mark_hot(void) { plat_spin_hot_ = 1; }
static inline int plat_spin_is_hot(void) { return plat_spin_hot_; }
static inline int64_t plat_spinhot_ns(void) {
    int64_t h = __atomic_load_n(&plat_spinhot_ns_, __ATOMIC_RELAXED);
    if (h < 0) {
        const char *e = getenv("SMO_SPINHOT_US");
        h = (e ? atoll(e) : PLAT_SPINHOT_DEFAULT_US) * 1000;
        int64_t unset = -1;
        __atomic_compare_exchange_n(&plat_spinhot_ns_, &unset, h, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED);
        h = __atomic_load_n(&plat_spinhot_ns_, __ATOMIC_RELAXED);
    }
    return h;
}
static inline void plat_spinhot_set_ns(int64_t ns) { __atomic_store_n(&plat_spinhot_ns_, ns, __ATOMIC_RELAXED); }
#else
static inline void plat_spin_mark_hot(void) {}
static inline int plat_spin_is_hot(void) { return 0; }
static inline int64_t plat_spinhot_ns(void) { return 0; }
#endif
static inline int64_t plat_spin_ns(void) {
    static int64_t ns = -1;
    if (ns < 0) {
        const char *e = getenv("SMO_SPINWAIT_US");
        ns = (e ? atoll(e) : PLAT_SPIN_DEFAULT_US) * 1000;
    }
    return plat_spin_is_hot() ? plat_spinhot_ns() : ns;
}
/* Spins while cond holds, at most plat_spin_ns(); cond reads shared state with atomic loads. */
#define PLAT_SPIN_WHILE(cond) do { \
        int64_t ps_ns_ = plat_spin_ns(); \
        if (ps_ns_ > 0) { \
            int64_t ps_end_ = plat_qpc() + ps_ns_ * plat_qpf() / 1000000000; \
            for (unsigned ps_i_ = 1; (cond); ps_i_++) { plat_cpu_relax(); if (!(ps_i_ & 63) && plat_qpc() >= ps_end_) break; } \
        } \
    } while (0)

/* base address of the loaded module (exe, DLL, shared object) that contains addr; 0 if none */
uintptr_t plat_module_base(const void *addr);

/* ---------------------------------------------------------------- aligned heap blocks (_aligned_malloc and friends) */
#ifdef _WIN32
#define plat_aligned_malloc(size, align) _aligned_malloc((size), (align))
#define plat_aligned_free(p) _aligned_free(p)
#define plat_aligned_realloc(p, size, align) _aligned_realloc((p), (size), (align))
#define plat_aligned_msize(p, align) _aligned_msize((p), (align), 0)
#else
void *plat_aligned_malloc(size_t size, size_t align);
void plat_aligned_free(void *p);
void *plat_aligned_realloc(void *p, size_t size, size_t align);
size_t plat_aligned_msize(void *p, size_t align);
#endif

/* ---------------------------------------------------------------- virtual memory (plat_posix.c / plat_win32.c) */
enum { PLAT_PROT_NONE = 0, PLAT_PROT_R = 1, PLAT_PROT_W = 2, PLAT_PROT_X = 4, PLAT_PROT_RW = 3 };
/* Reserve (and commit, when prot is not NONE) size bytes. With addr != NULL the mapping must land exactly there or
 * the call fails (NULL); never replaces an existing mapping. */
void *plat_vm_alloc(void *addr, size_t size, int prot);
int plat_vm_protect(void *addr, size_t size, int prot);          /* 1 on success */
void plat_vm_free(void *addr, size_t size);                       /* release a whole plat_vm_alloc range (size 0: as allocated) */
void plat_vm_decommit(void *addr, size_t size);                   /* drop the pages, keep the range reserved */
typedef struct { uintptr_t base; size_t size; int prot; int committed; } plat_vm_info;
int plat_vm_query(const void *addr, plat_vm_info *out);           /* 1 if addr lies in a mapping; fills out */
int plat_readable(const void *addr, size_t n);                    /* the n bytes at addr can be read now */
size_t plat_page_size(void);

/* Shared memory that can be mapped at more than one address (graphics memory pools alias heap pages). */
typedef struct plat_shm plat_shm;
plat_shm *plat_shm_create(size_t size);
void *plat_shm_map(plat_shm *s, size_t offset, size_t size, void *addr);   /* addr NULL = anywhere; else exactly there */
void plat_shm_unmap(void *addr, size_t size);

/* ---------------------------------------------------------------- threads */
typedef struct plat_thread plat_thread;
typedef void (*plat_thread_fn)(void *arg);
plat_thread *plat_thread_start(plat_thread_fn fn, void *arg, size_t stack_size, const char *name);
void plat_thread_join(plat_thread *t);                            /* waits for the end and frees t */
int plat_thread_wait(plat_thread *t, uint32_t ms);                /* 1 when t has ended (t stays valid), 0 time-out */
/* a thread function in the Win32 form (returns a code nobody reads); NULL fn is never started */
plat_thread *plat_thread_start32(uint32_t (*fn)(void *arg), void *arg, size_t stack_size, const char *name);
int plat_threads_wait_all(plat_thread **t, int n, uint32_t ms);  /* 1 when all n (NULL entries skipped) have ended */
int plat_thread_get_priority(void);                              /* the calling thread's last plat_thread_set_priority level */
void plat_thread_detach(plat_thread *t);
uint32_t plat_thread_id(plat_thread *t);
/* priority levels as THREAD_PRIORITY_*; off Windows they are nice values of the thread (plat_posix.c) */
enum { PLAT_PRIO_LOWEST = -2, PLAT_PRIO_BELOW = -1, PLAT_PRIO_NORMAL = 0, PLAT_PRIO_ABOVE = 1, PLAT_PRIO_HIGHEST = 2,
       PLAT_PRIO_TIME_CRITICAL = 3 };
void plat_thread_set_priority(plat_thread *t, int prio);           /* t NULL = the calling thread */
void plat_thread_set_affinity(plat_thread *t, uint64_t cpu_mask);    /* t NULL = the calling thread */
void plat_thread_set_name(const char *name);                          /* the calling thread */
int plat_cpu_count(void);

/* ---------------------------------------------------------------- events (manual or auto reset) */
typedef struct plat_event plat_event;
plat_event *plat_event_create(int manual_reset, int initial);
void plat_event_set(plat_event *e);
void plat_event_reset(plat_event *e);
int plat_event_wait(plat_event *e, uint32_t ms);                  /* 1 signalled, 0 time-out */
void plat_event_destroy(plat_event *e);

/* ---------------------------------------------------------------- counting semaphores */
typedef struct plat_sem plat_sem;
plat_sem *plat_sem_create(int initial);
void plat_sem_release(plat_sem *s, int n);
int plat_sem_wait(plat_sem *s, uint32_t ms);                      /* 1 taken, 0 time-out */

int plat_cpu_now(void);                                           /* the processor the calling thread runs on */

/* ---------------------------------------------------------------- one-time initialisation */
typedef struct { volatile int state; plat_mutex *lock_unused; } plat_once;
#define PLAT_ONCE_INIT {0, 0}
void plat_once_run(plat_once *o, void (*fn)(void));

/* ---------------------------------------------------------------- files and folders */
int plat_mkdir(const char *path);                                 /* 1 made or already there */
int plat_mkdirs(const char *path);                                /* every missing folder of the path */
int plat_is_dir(const char *path);
int plat_is_file(const char *path);
int64_t plat_file_size(const char *path);                         /* -1 if missing */
int plat_remove(const char *path);                                /* file or empty folder; 1 on success */
int plat_rename(const char *from, const char *to);                /* replaces to; 1 on success */
int plat_exists(const char *path);                                /* a file or a folder */
int plat_file_stamp(const char *path, uint64_t *size, uint64_t *mtime);   /* 1 if it exists: size and modification time */
#ifdef _WIN32
#define plat_fseek64 _fseeki64
#define plat_ftell64 _ftelli64
#else
#define plat_fseek64 fseeko
#define plat_ftell64 ftello
#endif
/* copies n bytes from src when all of them are readable now (0 otherwise, nothing faults): ReadProcessMemory on itself */
int plat_read_safe(void *dst, const void *src, size_t n);
int plat_fsync(FILE *f);
typedef struct plat_dir plat_dir;
plat_dir *plat_dir_open(const char *path);
/* next entry (never "." or ".."): name valid until the next call; is_dir set. 0 at the end */
int plat_dir_next(plat_dir *d, const char **name, int *is_dir, int64_t *size);
void plat_dir_close(plat_dir *d);
const char *plat_exe_dir(void);                                   /* folder of the program (Android: the data root) */
void plat_set_data_root(const char *path);                        /* Android: set once by android_main */
const char *plat_data_root(void);                                 /* folder that holds romfs/, userdata/, config/ */
const char *plat_temp_dir(void);

/* ---------------------------------------------------------------- the game's window
 * Windows: an HWND made by display.c; Android: the activity's ANativeWindow (android_main.c), which can go away when the
 * app is in the background and come back as another window. */
void *plat_window_wait(void);                                     /* blocks until there is a window; returns it */
int plat_window_size(void *win, int *width, int *height);         /* the drawable size in pixels; 0 if unknown */
int plat_window_minimized(void *win);                             /* not visible (minimised, app in the background) */
void plat_window_set_title(void *win, const char *title);         /* Android: nothing (a log line now and then) */

/* ---------------------------------------------------------------- sound output (Android: AAudio, android_main.c)
 * The device pulls: fill() is called on the device's own thread for `frames` interleaved 16-bit frames and must not
 * block. Returns 1 when the stream runs. */
typedef void (*plat_audio_fill)(int16_t *out, int frames, void *ud);
int plat_audio_open(int rate, int channels, plat_audio_fill fill, void *ud);

/* ---------------------------------------------------------------- keyboard
 * Keys are numbered as Windows virtual keys on every host (controllers.ini and the input sequences name them so):
 * letters and digits are their capital ASCII codes, the rest below. Android: a hardware keyboard, mapped by
 * android_main.c. plat_window_focused: the game's window has the keyboard (Windows: the foreground window is ours). */
enum { PLAT_VK_BACK = 0x08, PLAT_VK_TAB = 0x09, PLAT_VK_RETURN = 0x0D, PLAT_VK_SHIFT = 0x10, PLAT_VK_CONTROL = 0x11,
       PLAT_VK_ESCAPE = 0x1B, PLAT_VK_SPACE = 0x20, PLAT_VK_LEFT = 0x25, PLAT_VK_UP = 0x26, PLAT_VK_RIGHT = 0x27,
       PLAT_VK_DOWN = 0x28, PLAT_VK_F1 = 0x70, PLAT_VK_F11 = 0x7A, PLAT_VK_F12 = 0x7B };
int plat_key_down(int vk);
int plat_window_focused(void);

/* ---------------------------------------------------------------- shared libraries */
void *plat_dlopen(const char *name);
void *plat_dlsym(void *lib, const char *name);

/* ---------------------------------------------------------------- process memory (reports) */
void plat_process_memory(size_t *resident, size_t *private_bytes, size_t *peak_resident);
double plat_process_age_s(void);                                  /* seconds since the process started */

/* ---------------------------------------------------------------- logging and crashes */
void plat_log_init(const char *log_path);                         /* stdout/stderr also to the platform log */
void plat_install_crash_handler(void (*report)(const char *what, uintptr_t fault_addr, uintptr_t pc));

#endif
