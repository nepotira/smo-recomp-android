/* plat.h on POSIX hosts (Android, Linux): mmap, pthreads, dirent, signals.
 * Android port (2026-10-27). On Android, stdout and stderr also go to logcat (tag "smo"), line by line. */
#ifndef _WIN32
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "plat.h"

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#ifdef __ANDROID__
#include <android/log.h>
#endif

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

#ifdef __ANDROID__
static void log_drain(void);                        /* below: the log pump has written everything before the process goes */
#endif
void plat_exit(int code) {
    fflush(stdout);
    fflush(stderr);
#ifdef __ANDROID__
    log_drain();
#endif
    _exit(code);
}

void plat_sleep_ns(int64_t ns) {
    if (ns <= 0) { sched_yield(); return; }
    struct timespec t = {(time_t)(ns / 1000000000), (long)(ns % 1000000000)};
    while (nanosleep(&t, &t) != 0 && errno == EINTR) {}
}

/* ---------------------------------------------------------------- aligned heap blocks
 * Bionic's and glibc's malloc return 16-byte aligned blocks on 64-bit hosts, so alignments up to 16 are plain malloc
 * (and realloc keeps them); larger ones come from posix_memalign. free() releases both. */
#include <malloc.h>
void *plat_aligned_malloc(size_t size, size_t align) {
    if (align <= 16) return malloc(size);
    void *p = NULL;
    return posix_memalign(&p, align, size) == 0 ? p : NULL;
}
void plat_aligned_free(void *p) { free(p); }
void *plat_aligned_realloc(void *p, size_t size, size_t align) {
    if (align <= 16) return realloc(p, size);
    void *n = plat_aligned_malloc(size, align);
    if (n && p) { size_t old = malloc_usable_size(p); memcpy(n, p, old < size ? old : size); free(p); }
    return n;
}
size_t plat_aligned_msize(void *p, size_t align) { (void)align; return malloc_usable_size(p); }

/* ---------------------------------------------------------------- virtual memory */
static int to_prot(int p) {
    return (p & PLAT_PROT_R ? PROT_READ : 0) | (p & PLAT_PROT_W ? PROT_WRITE : 0) | (p & PLAT_PROT_X ? PROT_EXEC : 0);
}

size_t plat_page_size(void) {
    static size_t ps;
    if (!ps) ps = (size_t)sysconf(_SC_PAGESIZE);
    return ps;
}

/* The size of every range plat_vm_alloc made, so plat_vm_free(addr, 0) can release it whole (as VirtualFree with
 * MEM_RELEASE does); an open-addressing table keyed by the base address. */
#define VMREG_SLOTS 65536
static struct { uintptr_t base; size_t size; } g_vmreg[VMREG_SLOTS];
static pthread_mutex_t g_vmreg_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned vmreg_hash(uintptr_t b) { return (unsigned)((b >> 12) * 2654435761u) & (VMREG_SLOTS - 1); }
static void vmreg_put(uintptr_t base, size_t size) {
    pthread_mutex_lock(&g_vmreg_lock);
    for (unsigned i = vmreg_hash(base), k = 0; k < VMREG_SLOTS; k++, i = (i + 1) & (VMREG_SLOTS - 1))
        if (!g_vmreg[i].base || g_vmreg[i].base == base || g_vmreg[i].base == 1) { g_vmreg[i].base = base; g_vmreg[i].size = size; break; }
    pthread_mutex_unlock(&g_vmreg_lock);
}
static size_t vmreg_take(uintptr_t base) {
    size_t size = 0;
    pthread_mutex_lock(&g_vmreg_lock);
    for (unsigned i = vmreg_hash(base), k = 0; k < VMREG_SLOTS && g_vmreg[i].base; k++, i = (i + 1) & (VMREG_SLOTS - 1))
        if (g_vmreg[i].base == base) { size = g_vmreg[i].size; g_vmreg[i].base = 1; break; }     /* 1: a deleted slot */
    pthread_mutex_unlock(&g_vmreg_lock);
    return size;
}

void *plat_vm_alloc(void *addr, size_t size, int prot) {
    int flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | (addr ? MAP_FIXED_NOREPLACE : 0);
    void *p = mmap(addr, size, to_prot(prot), flags, -1, 0);
    if (p == MAP_FAILED) return NULL;
    if (addr && p != addr) {               /* a kernel without MAP_FIXED_NOREPLACE took addr as a hint */
        munmap(p, size);
        return NULL;
    }
    vmreg_put((uintptr_t)p, size);
    return p;
}

int plat_vm_protect(void *addr, size_t size, int prot) {
    uintptr_t ps = plat_page_size(), a = (uintptr_t)addr & ~(ps - 1), e = ((uintptr_t)addr + size + ps - 1) & ~(ps - 1);
    return mprotect((void *)a, e - a, to_prot(prot)) == 0;
}

void plat_vm_free(void *addr, size_t size) {
    size_t made = vmreg_take((uintptr_t)addr);
    if (!size) size = made;
    if (size) munmap(addr, size);
}

void plat_vm_decommit(void *addr, size_t size) {
    madvise(addr, size, MADV_DONTNEED);
    mprotect(addr, size, PROT_NONE);
}

/* /proc/self/maps as a sorted table, read again when a query misses it or the table is older than 50 ms (the game maps
 * and unmaps while it runs). Queries are for diagnostics and the renderer's look for shader containers in guest memory,
 * not for every memory access. */
typedef struct { uintptr_t lo, hi; int prot; } MapEnt;
static MapEnt *g_maps;
static int g_nmaps, g_maps_cap;
static int64_t g_maps_time;
static pthread_mutex_t g_maps_lock = PTHREAD_MUTEX_INITIALIZER;
static void maps_read(void) {
    FILE *f = fopen("/proc/self/maps", "r");
    char line[512];
    g_nmaps = 0;
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        unsigned long long lo, hi;
        char perms[8] = {0};
        if (sscanf(line, "%llx-%llx %7s", &lo, &hi, perms) != 3) continue;
        if (g_nmaps == g_maps_cap) {
            int cap = g_maps_cap ? 2 * g_maps_cap : 4096;
            MapEnt *n = realloc(g_maps, (size_t)cap * sizeof *n);
            if (!n) break;
            g_maps = n; g_maps_cap = cap;
        }
        g_maps[g_nmaps].lo = (uintptr_t)lo;
        g_maps[g_nmaps].hi = (uintptr_t)hi;
        g_maps[g_nmaps].prot = (perms[0] == 'r' ? PLAT_PROT_R : 0) | (perms[1] == 'w' ? PLAT_PROT_W : 0) | (perms[2] == 'x' ? PLAT_PROT_X : 0);
        g_nmaps++;
    }
    fclose(f);
    g_maps_time = plat_now_ns();
}
static int maps_find(uintptr_t a) {
    int lo = 0, hi = g_nmaps - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (a < g_maps[mid].lo) hi = mid - 1;
        else if (a >= g_maps[mid].hi) lo = mid + 1;
        else return mid;
    }
    return -1;
}
int plat_vm_query(const void *addr, plat_vm_info *out) {
    uintptr_t a = (uintptr_t)addr;
    pthread_mutex_lock(&g_maps_lock);
    int i = plat_now_ns() - g_maps_time < 50000000 ? maps_find(a) : -1;
    if (i < 0) { maps_read(); i = maps_find(a); }
    if (i >= 0) {
        /* adjacent entries with the same protection are one region for the callers (VirtualQuery merges them too) */
        int b = i, e = i;
        while (b > 0 && g_maps[b - 1].hi == g_maps[b].lo && g_maps[b - 1].prot == g_maps[i].prot) b--;
        while (e + 1 < g_nmaps && g_maps[e + 1].lo == g_maps[e].hi && g_maps[e + 1].prot == g_maps[i].prot) e++;
        out->base = g_maps[b].lo;
        out->size = g_maps[e].hi - g_maps[b].lo;
        out->prot = g_maps[i].prot;
        out->committed = out->prot != 0;
    }
    pthread_mutex_unlock(&g_maps_lock);
    return i >= 0;
}

/* A readable range: write() from it into a pipe fails with EFAULT instead of faulting. One pipe per thread. */
int plat_readable(const void *addr, size_t n) {
    static _Thread_local int fds[2] = {-1, -1};
    if ((uintptr_t)addr < 0x10000) return 0;
    if (fds[0] < 0 && pipe2(fds, O_NONBLOCK | O_CLOEXEC) != 0) return 0;
    const char *p = addr;
    size_t ps = plat_page_size();
    char sink[64];
    while (n > 0) {
        size_t chunk = ps - ((uintptr_t)p & (ps - 1));
        if (chunk > n) chunk = n;
        ssize_t w = write(fds[1], p, 1);                     /* one byte per page is enough */
        if (w < 0) return 0;
        while (read(fds[0], sink, sizeof sink) > 0) {}
        if (chunk == n) {                                     /* the last byte of the range, too */
            if (write(fds[1], p + chunk - 1, 1) < 0) return 0;
            while (read(fds[0], sink, sizeof sink) > 0) {}
        }
        p += chunk;
        n -= chunk;
    }
    return 1;
}

#include <sys/uio.h>
int plat_read_safe(void *dst, const void *src, size_t n) {
    struct iovec l = {dst, n}, r = {(void *)src, n};
    return process_vm_readv(getpid(), &l, 1, &r, 1, 0) == (ssize_t)n;
}

void plat_process_memory(size_t *resident, size_t *private_bytes, size_t *peak_resident) {
    unsigned long long size = 0, res = 0, shared = 0;
    FILE *f = fopen("/proc/self/statm", "r");
    if (f) { if (fscanf(f, "%llu %llu %llu", &size, &res, &shared) != 3) size = res = shared = 0; fclose(f); }
    size_t ps = plat_page_size();
    if (resident) *resident = (size_t)(res * ps);
    if (private_bytes) *private_bytes = (size_t)((res > shared ? res - shared : 0) * ps);
    if (peak_resident) {
        *peak_resident = 0;
        char line[128];
        if ((f = fopen("/proc/self/status", "r"))) {
            while (fgets(line, sizeof line, f)) { unsigned long long kb; if (sscanf(line, "VmHWM: %llu kB", &kb) == 1) *peak_resident = (size_t)(kb << 10); }
            fclose(f);
        }
    }
}

struct plat_shm { int fd; size_t size; };

plat_shm *plat_shm_create(size_t size) {
    int fd = (int)syscall(SYS_memfd_create, "smo-shm", 1u /* MFD_CLOEXEC */);
    if (fd < 0) return NULL;
    if (ftruncate(fd, (off_t)size) != 0) { close(fd); return NULL; }
    plat_shm *s = calloc(1, sizeof *s);
    s->fd = fd;
    s->size = size;
    return s;
}

void *plat_shm_map(plat_shm *s, size_t offset, size_t size, void *addr) {
    void *p = mmap(addr, size, PROT_READ | PROT_WRITE, MAP_SHARED | (addr ? MAP_FIXED : 0), s->fd, (off_t)offset);
    return p == MAP_FAILED ? NULL : p;
}

void plat_shm_unmap(void *addr, size_t size) { munmap(addr, size); }

/* ---------------------------------------------------------------- threads */
struct plat_thread { pthread_t t; plat_thread_fn fn; void *arg; volatile uint32_t tid; char name[16]; plat_event *done; };

static void *thread_main(void *p) {
    plat_thread *t = p;
    {   /* A thread held on one processor keeps it for itself: its new threads do not inherit that. Measured on a Galaxy
         * S24 Ultra (2026-11-03): the render thread, held on the Cortex-X4, started the pipeline workers (6), the watchdog
         * and the pf_threads helper, and all of them ran on its one processor. They get every other processor now;
         * threads that choose their own (the render thread, the console's cores: os_threads.c) still do so after. */
        cpu_set_t s;
        CPU_ZERO(&s);
        if (sched_getaffinity(0, sizeof s, &s) == 0 && CPU_COUNT(&s) == 1) {
            int n = plat_cpu_count(), one = -1;
            for (int i = 0; i < n && i < CPU_SETSIZE; i++) if (CPU_ISSET(i, &s)) one = i;
            if (n > 1 && one >= 0) {
                CPU_ZERO(&s);
                for (int i = 0; i < n && i < CPU_SETSIZE; i++) if (i != one) CPU_SET(i, &s);
                sched_setaffinity(0, sizeof s, &s);
            }
        }
    }
    t->tid = plat_tid();
    if (t->name[0]) pthread_setname_np(pthread_self(), t->name);
    t->fn(t->arg);
    plat_event_set(t->done);
    return NULL;
}

plat_thread *plat_thread_start(plat_thread_fn fn, void *arg, size_t stack_size, const char *name) {
    plat_thread *t = calloc(1, sizeof *t);
    t->fn = fn;
    t->arg = arg;
    t->done = plat_event_create(1, 0);
    if (name) snprintf(t->name, sizeof t->name, "%s", name);
    pthread_attr_t a;
    pthread_attr_init(&a);
    if (stack_size) pthread_attr_setstacksize(&a, stack_size < 65536 ? 65536 : stack_size);
    int r = pthread_create(&t->t, &a, thread_main, t);
    pthread_attr_destroy(&a);
    if (r) { plat_event_destroy(t->done); free(t); return NULL; }
    while (!t->tid) sched_yield();                       /* plat_thread_id is valid as soon as start returns */
    return t;
}

void plat_thread_join(plat_thread *t) { if (t) { pthread_join(t->t, NULL); plat_event_destroy(t->done); free(t); } }
int plat_thread_wait(plat_thread *t, uint32_t ms) { return plat_event_wait(t->done, ms); }
void plat_thread_detach(plat_thread *t) { if (t) pthread_detach(t->t); }
uint32_t plat_thread_id(plat_thread *t) { return t ? t->tid : plat_tid(); }

/* Linux has no per-thread priority for SCHED_OTHER threads beyond the nice value of the thread's task id. Android
 * lets an app raise its own threads to -8 or so (THREAD_PRIORITY_URGENT_DISPLAY); failures are ignored. */
static _Thread_local int t_prio;
void plat_thread_set_priority(plat_thread *t, int prio) {
    /* -2 .. 3: far apart, so that on one processor a higher level runs nearly always first (CFS weights: -19 is 650
     * times 10), as the console runs only the highest-priority ready thread of a core */
    static const int k_nice[6] = {10, 4, -2, -8, -14, -19};
    int i = prio < -2 ? 0 : prio > 3 ? 5 : prio + 2;
    setpriority(PRIO_PROCESS, (id_t)(t ? t->tid : plat_tid()), k_nice[i]);
    if (!t || t->tid == plat_tid()) t_prio = prio;
}
int plat_thread_get_priority(void) { return t_prio; }

typedef struct { uint32_t (*fn)(void *); void *arg; } Start32;
static void start32_main(void *p) { Start32 s = *(Start32 *)p; free(p); s.fn(s.arg); }
plat_thread *plat_thread_start32(uint32_t (*fn)(void *), void *arg, size_t stack_size, const char *name) {
    if (!fn) return NULL;
    Start32 *s = malloc(sizeof *s);
    s->fn = fn; s->arg = arg;
    plat_thread *t = plat_thread_start(start32_main, s, stack_size, name);
    if (!t) free(s);
    return t;
}
int plat_threads_wait_all(plat_thread **t, int n, uint32_t ms) {
    int64_t end = plat_now_ns() + (int64_t)ms * 1000000;
    for (int i = 0; i < n; i++) {
        if (!t[i]) continue;
        int64_t left = (end - plat_now_ns()) / 1000000;
        if (!plat_thread_wait(t[i], ms == PLAT_INFINITE ? PLAT_INFINITE : left > 0 ? (uint32_t)left : 0)) return 0;
    }
    return 1;
}

void plat_thread_set_affinity(plat_thread *t, uint64_t cpu_mask) {
    cpu_set_t s;
    CPU_ZERO(&s);
    for (int i = 0; i < 64 && i < CPU_SETSIZE; i++) if (cpu_mask >> i & 1) CPU_SET(i, &s);
    sched_setaffinity((pid_t)(t ? t->tid : plat_tid()), sizeof s, &s);
}

void plat_thread_set_name(const char *name) {
    char n[16];
    snprintf(n, sizeof n, "%s", name);
    pthread_setname_np(pthread_self(), n);
}

int plat_cpu_count(void) {
    long n = sysconf(_SC_NPROCESSORS_CONF);
    return n > 0 ? (int)n : 1;
}

/* ---------------------------------------------------------------- events */
struct plat_event { pthread_mutex_t m; pthread_cond_t c; int manual, set; };

plat_event *plat_event_create(int manual_reset, int initial) {
    plat_event *e = calloc(1, sizeof *e);
    pthread_mutex_init(&e->m, NULL);
    pthread_cond_init(&e->c, NULL);
    e->manual = manual_reset;
    e->set = initial;
    return e;
}

void plat_event_set(plat_event *e) {
    pthread_mutex_lock(&e->m);
    e->set = 1;
    if (e->manual) pthread_cond_broadcast(&e->c);
    else pthread_cond_signal(&e->c);
    pthread_mutex_unlock(&e->m);
}

void plat_event_reset(plat_event *e) {
    pthread_mutex_lock(&e->m);
    e->set = 0;
    pthread_mutex_unlock(&e->m);
}

_Thread_local int plat_spin_hot_;                     /* plat.h: spinning before blocking */
int64_t plat_spinhot_ns_ = -1;                        /* plat.h: the hot threads' spin; -1 until SMO_SPINHOT_US is read */

int plat_event_wait(plat_event *e, uint32_t ms) {
    if (ms) PLAT_SPIN_WHILE(!__atomic_load_n(&e->set, __ATOMIC_ACQUIRE));         /* plat.h: spinning before blocking */
    pthread_mutex_lock(&e->m);
    if (!e->set && ms) {
        struct timespec t;
        clock_gettime(CLOCK_MONOTONIC, &t);
        t.tv_sec += ms / 1000;
        t.tv_nsec += (long)(ms % 1000) * 1000000L;
        if (t.tv_nsec >= 1000000000L) { t.tv_sec++; t.tv_nsec -= 1000000000L; }
        while (!e->set) {
            if (ms == PLAT_INFINITE) pthread_cond_wait(&e->c, &e->m);
            else if (pthread_cond_clockwait(&e->c, &e->m, CLOCK_MONOTONIC, &t) == ETIMEDOUT) break;
        }
    }
    int ok = e->set;
    if (ok && !e->manual) e->set = 0;
    pthread_mutex_unlock(&e->m);
    return ok;
}

void plat_event_destroy(plat_event *e) {
    if (!e) return;
    pthread_cond_destroy(&e->c);
    pthread_mutex_destroy(&e->m);
    free(e);
}

/* ---------------------------------------------------------------- once */
static pthread_mutex_t g_once_lock = PTHREAD_MUTEX_INITIALIZER;

void plat_once_run(plat_once *o, void (*fn)(void)) {
    if (__atomic_load_n(&o->state, __ATOMIC_ACQUIRE) == 2) return;
    pthread_mutex_lock(&g_once_lock);              /* one lock for all: these run a handful of times per process */
    if (o->state != 2) {
        o->state = 1;
        fn();
        __atomic_store_n(&o->state, 2, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&g_once_lock);
}

/* ---------------------------------------------------------------- files */
int plat_mkdir(const char *path) { return mkdir(path, 0775) == 0 || errno == EEXIST; }

int plat_mkdirs(const char *path) {
    char p[PLAT_MAX_PATH];
    snprintf(p, sizeof p, "%s", path);
    for (char *s = p + 1; *s; s++) {
        if (*s == '/' || *s == '\\') { char c = *s; *s = 0; plat_mkdir(p); *s = c; }
    }
    return plat_mkdir(p);
}

int plat_is_dir(const char *path) { struct stat st; return stat(path, &st) == 0 && S_ISDIR(st.st_mode); }
int plat_is_file(const char *path) { struct stat st; return stat(path, &st) == 0 && S_ISREG(st.st_mode); }
int64_t plat_file_size(const char *path) { struct stat st; return stat(path, &st) == 0 ? (int64_t)st.st_size : -1; }
int plat_remove(const char *path) { return remove(path) == 0; }
int plat_rename(const char *from, const char *to) { return rename(from, to) == 0; }
int plat_fsync(FILE *f) { fflush(f); return fsync(fileno(f)) == 0; }
int plat_exists(const char *path) { struct stat st; return stat(path, &st) == 0; }
int plat_file_stamp(const char *path, uint64_t *size, uint64_t *mtime) {
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    if (size) *size = (uint64_t)st.st_size;
    if (mtime) *mtime = (uint64_t)st.st_mtim.tv_sec * 1000000000ull + (uint64_t)st.st_mtim.tv_nsec;
    return 1;
}

struct plat_dir { DIR *d; char path[PLAT_MAX_PATH]; };

plat_dir *plat_dir_open(const char *path) {
    DIR *d = opendir(path);
    if (!d) return NULL;
    plat_dir *r = calloc(1, sizeof *r);
    r->d = d;
    snprintf(r->path, sizeof r->path, "%s", path);
    return r;
}

int plat_dir_next(plat_dir *d, const char **name, int *is_dir, int64_t *size) {
    struct dirent *e;
    while ((e = readdir(d->d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char full[PLAT_MAX_PATH * 2];
        struct stat st;
        snprintf(full, sizeof full, "%s/%s", d->path, e->d_name);
        int ok = stat(full, &st) == 0;
        *name = e->d_name;
        if (is_dir) *is_dir = ok ? S_ISDIR(st.st_mode) : e->d_type == DT_DIR;
        if (size) *size = ok ? (int64_t)st.st_size : 0;
        return 1;
    }
    return 0;
}

void plat_dir_close(plat_dir *d) { if (d) { closedir(d->d); free(d); } }

static char g_data_root[PLAT_MAX_PATH];
void plat_set_data_root(const char *path) { snprintf(g_data_root, sizeof g_data_root, "%s", path); }

const char *plat_exe_dir(void) {
    static char dir[PLAT_MAX_PATH];
    if (g_data_root[0]) return g_data_root;
    if (!dir[0]) {
        ssize_t n = readlink("/proc/self/exe", dir, sizeof dir - 1);
        if (n > 0) { dir[n] = 0; char *s = strrchr(dir, '/'); if (s) *s = 0; }
        else snprintf(dir, sizeof dir, ".");
    }
    return dir;
}

const char *plat_data_root(void) { return g_data_root[0] ? g_data_root : plat_exe_dir(); }

const char *plat_temp_dir(void) {
    static char dir[PLAT_MAX_PATH];
    if (!dir[0]) {
        const char *t = getenv("TMPDIR");
        if (t && *t) snprintf(dir, sizeof dir, "%s", t);
        else snprintf(dir, sizeof dir, "%s/tmp", plat_data_root());
        plat_mkdirs(dir);
    }
    return dir;
}

/* ---------------------------------------------------------------- shared libraries */
void *plat_dlopen(const char *name) { return dlopen(name, RTLD_NOW | RTLD_LOCAL); }
void *plat_dlsym(void *lib, const char *name) { return dlsym(lib ? lib : RTLD_DEFAULT, name); }

/* ---------------------------------------------------------------- logging */
#ifdef __ANDROID__
static int g_log_pipe[2];
static FILE *g_log_file;
static volatile int g_log_pumping;

static void *log_pump(void *arg) {
    (void)arg;
    char buf[4096], line[1024];
    size_t used = 0;
    ssize_t n;
    __atomic_store_n(&g_log_pumping, 1, __ATOMIC_RELEASE);
    while ((n = read(g_log_pipe[0], buf, sizeof buf)) > 0) {
        if (g_log_file) { fwrite(buf, 1, (size_t)n, g_log_file); fflush(g_log_file); }
        for (ssize_t i = 0; i < n; i++) {
            if (buf[i] == '\n' || used == sizeof line - 1) {
                line[used] = 0;
                __android_log_write(ANDROID_LOG_INFO, "smo", line);
                used = 0;
                if (buf[i] == '\n') continue;
            }
            line[used++] = buf[i];
        }
    }
    if (used) { line[used] = 0; __android_log_write(ANDROID_LOG_INFO, "smo", line); }
    __atomic_store_n(&g_log_pumping, 0, __ATOMIC_RELEASE);
    return NULL;
}
/* Android port: plat_exit called _exit at once, and what the pump had not read yet was lost (the reason of a stop is
 * printed just before it: a stop after the opening cutscene left no trace). Every writing end of the pipe is closed, so
 * the pump reads to the end and stops; at most 2 s are waited. */
static void log_drain(void) {
    if (!__atomic_load_n(&g_log_pumping, __ATOMIC_ACQUIRE)) return;
    close(1); close(2); close(g_log_pipe[1]);
    for (int i = 0; i < 200 && __atomic_load_n(&g_log_pumping, __ATOMIC_ACQUIRE); i++) usleep(10000);
}
#endif

void plat_log_init(const char *log_path) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
#ifdef __ANDROID__
    if (log_path) g_log_file = fopen(log_path, "w");
    if (pipe(g_log_pipe) == 0) {
        dup2(g_log_pipe[1], 1);
        dup2(g_log_pipe[1], 2);
        pthread_t t;
        pthread_create(&t, NULL, log_pump, NULL);
        pthread_detach(t);
    }
#else
    if (log_path) { freopen(log_path, "w", stdout); dup2(fileno(stdout), 2); }
#endif
}

/* ---------------------------------------------------------------- crashes */
static void (*g_report)(const char *, uintptr_t, uintptr_t);

uintptr_t plat_crash_link;
static void crash_handler(int sig, siginfo_t *si, void *uc_) {
    uintptr_t pc = 0;
#if defined(__aarch64__)
    pc = (uintptr_t)((ucontext_t *)uc_)->uc_mcontext.pc;
#elif defined(__x86_64__)
    pc = (uintptr_t)((ucontext_t *)uc_)->uc_mcontext.gregs[REG_RIP];
#endif
    /* the caller of the faulting code: after a call through a null or bad pointer the pc says nothing, the link does */
#if defined(__aarch64__)
    plat_crash_link = (uintptr_t)((ucontext_t *)uc_)->uc_mcontext.regs[30];
#elif defined(__x86_64__)
    {
        uintptr_t sp = (uintptr_t)((ucontext_t *)uc_)->uc_mcontext.gregs[REG_RSP];
        if (sp && plat_readable((const void *)sp, sizeof(uintptr_t))) plat_crash_link = *(const uintptr_t *)sp;
    }
#endif
    static const char *names[32] = {[SIGSEGV] = "SIGSEGV (access violation)", [SIGBUS] = "SIGBUS", [SIGILL] = "SIGILL",
                                    [SIGFPE] = "SIGFPE", [SIGABRT] = "SIGABRT", [SIGTRAP] = "SIGTRAP"};
    signal(sig, SIG_DFL);
    if (g_report) g_report(sig < 32 && names[sig] ? names[sig] : "signal", (uintptr_t)si->si_addr, pc);
    fflush(stdout);
    raise(sig);
}

void plat_install_crash_handler(void (*report)(const char *, uintptr_t, uintptr_t)) {
    g_report = report;
    static char alt[1 << 16];
    stack_t ss = {.ss_sp = alt, .ss_size = sizeof alt, .ss_flags = 0};
    sigaltstack(&ss, NULL);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    int sigs[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGTRAP};
    for (unsigned i = 0; i < sizeof sigs / sizeof *sigs; i++) sigaction(sigs[i], &sa, NULL);
}
#endif

#ifndef _WIN32
uintptr_t plat_module_base(const void *addr) {
    Dl_info di;
    return dladdr(addr, &di) ? (uintptr_t)di.dli_fbase : 0;
}
#endif

#ifndef _WIN32
/* ---------------------------------------------------------------- counting semaphores */
struct plat_sem { pthread_mutex_t m; pthread_cond_t c; int count; };

plat_sem *plat_sem_create(int initial) {
    plat_sem *s = calloc(1, sizeof *s);
    pthread_mutex_init(&s->m, NULL);
    pthread_cond_init(&s->c, NULL);
    s->count = initial;
    return s;
}

void plat_sem_release(plat_sem *s, int n) {
    pthread_mutex_lock(&s->m);
    s->count += n;
    if (n == 1) pthread_cond_signal(&s->c); else pthread_cond_broadcast(&s->c);
    pthread_mutex_unlock(&s->m);
}

int plat_sem_wait(plat_sem *s, uint32_t ms) {
    pthread_mutex_lock(&s->m);
    if (ms == PLAT_INFINITE) while (!s->count) pthread_cond_wait(&s->c, &s->m);
    else if (!s->count) {
        struct timespec t;
        clock_gettime(CLOCK_MONOTONIC, &t);
        t.tv_sec += ms / 1000;
        t.tv_nsec += (long)(ms % 1000) * 1000000L;
        if (t.tv_nsec >= 1000000000L) { t.tv_sec++; t.tv_nsec -= 1000000000L; }
        while (!s->count && pthread_cond_clockwait(&s->c, &s->m, CLOCK_MONOTONIC, &t) != ETIMEDOUT) {}
    }
    int ok = s->count > 0;
    if (ok) s->count--;
    pthread_mutex_unlock(&s->m);
    return ok;
}

int plat_cpu_now(void) { int c = sched_getcpu(); return c < 0 ? 0 : c; }
#endif

#ifndef _WIN32
double plat_process_age_s(void) {
    char buf[1024];
    double up = 0;
    unsigned long long start = 0;
    FILE *f = fopen("/proc/uptime", "r");
    if (f) { if (fscanf(f, "%lf", &up) != 1) up = 0; fclose(f); }
    f = fopen("/proc/self/stat", "r");
    if (f) {
        size_t n = fread(buf, 1, sizeof buf - 1, f);
        fclose(f);
        buf[n] = 0;
        char *p = strrchr(buf, ')');                   /* fields after the command name; starttime is field 22 */
        for (int field = 2; p && field < 22; field++) p = strchr(p + 1, ' ');
        if (p) start = strtoull(p + 1, NULL, 10);
    }
    long hz = sysconf(_SC_CLK_TCK);
    return start && hz > 0 ? up - (double)start / (double)hz : 0;
}
#endif
