/* nn::fs on host directories, plus the minimal nn::account and nn::nifm
 * answers the game needs to start: one local user, no network.
 *
 * MOUNT TABLE. A path is "name:/dir/file"; the name must have been mounted, as in the SDK's own mount table
 * ([sdk 0x1f4960] MountTable::Find: an unknown name is fs NotMounted 0x35f202; mounting a name twice is
 * MountNameAlreadyExists 0x7802). Mount kinds and their host folders:
 *   MountRom           -> %SMO_ROMFS% or ..\romfs                  (the game's extracted romfs, update applied; read-only)
 *   MountSaveData      -> <userdata>\save                           (journaled, see below)
 *   MountCacheStorage  -> <userdata>\cache                          (journaled like save data)
 * <userdata> is %SMO_USERDATA% or ..\userdata (tests point SMO_USERDATA at a copy of the user's save).
 *
 * SAVE DATA JOURNAL. On the console, writes to save data are not permanent until nn::fs::Commit: they go to a journal,
 * Commit applies all of them together, and uncommitted writes are gone at the next mount. Here:
 *   - <userdata>\save is the COMMITTED state and is never written outside Commit.
 *   - Mounting creates the working folder <userdata>\save.work.<pid>. A file is copied into it the first time it is
 *     opened for writing (copy-on-write) or created there; deletions are recorded (tombstones); every read, existence
 *     test and directory listing sees the working folder over the committed one, minus the tombstones.
 *   - Commit ([sdk 0x1f4490] FileSystemAccessor::Commit: fails with 0x327202 while a file of the mount is open with the
 *     write mode) is two-phase: (1) the changed files are flushed and the list of changes is written to
 *     <userdata>\save.journal.<pid>, flushed, and renamed to <userdata>\save.commit -- from this rename on the commit
 *     is decided; (2) the changes are applied (files moved over the committed ones, deletions done) and the marker is
 *     removed. A process killed in phase 1 leaves the committed state untouched; one killed in phase 2 leaves the
 *     marker, and the next mount finishes the apply (roll forward) before anything is read.
 *   - Unmount, or a later mount by any process, discards the working folder of a process that no longer runs.
 * Not reproduced: the console's limit on the journal SIZE (a Commit is needed before the journal fills; the game's own
 * save code already commits within it). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "plat.h"
#include "runtime.h"
#include "hostrt.h"

#define A0 (c->x[0])
#define A1 (c->x[1])
#define A2 (c->x[2])
#define A3 (c->x[3])
#define A4 (c->x[4])
#define RET(v) (c->x[0] = (uint64_t)(v))
#define STR(n) ((const char *)(uintptr_t)c->x[n])

/* ---- host files and folders (Android port: one set of calls on both hosts). PSEP is the host's path separator; the
 * relative paths of the mount table use it too. A guest FileHandle is the host handle (Windows) or the descriptor
 * offset by 2^32 (POSIX: never 0, and a stale value is recognisable). */
#ifdef _WIN32
#define PSEP '\\'
#define PSEP_S "\\"
typedef HANDLE FsH;
#define FSH_NONE INVALID_HANDLE_VALUE
static FsH fsh_open(const char *p, int write) { return CreateFileA(p, GENERIC_READ | (write ? GENERIC_WRITE : 0), FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL); }
static FsH fsh_create(const char *p) { return CreateFileA(p, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL); }
static void fsh_close(FsH h) { CloseHandle(h); }
static uint64_t fsh_pread(FsH h, uint64_t pos, void *buf, uint64_t size) {
    uint64_t total = 0;
    while (total < size) {
        OVERLAPPED ov = {0};
        uint64_t at = pos + total;
        ov.Offset = (uint32_t)at;
        ov.OffsetHigh = (uint32_t)(at >> 32);
        uint32_t chunk = size - total > 0x40000000 ? 0x40000000 : (uint32_t)(size - total), got = 0;
        if (!ReadFile(h, (char *)buf + total, chunk, (DWORD *)&got, &ov) || got == 0) break;
        total += got;
    }
    return total;
}
static int fsh_pwrite(FsH h, uint64_t pos, const void *buf, uint32_t size) {
    OVERLAPPED ov = {0};
    uint32_t put = 0;
    ov.Offset = (uint32_t)pos;
    ov.OffsetHigh = (uint32_t)(pos >> 32);
    return WriteFile(h, buf, size, (DWORD *)&put, &ov) && put == size;
}
static int fsh_size(FsH h, int64_t *out) { LARGE_INTEGER s; if (!GetFileSizeEx(h, &s)) return 0; *out = s.QuadPart; return 1; }
static int fsh_truncate(FsH h, int64_t size) { LARGE_INTEGER pos; pos.QuadPart = size; return SetFilePointerEx(h, pos, NULL, FILE_BEGIN) && SetEndOfFile(h); }
static void fsh_flush(FsH h) { FlushFileBuffers(h); }
static uint64_t fsh_guest(FsH h) { return (uint64_t)(uintptr_t)h; }
static FsH fsh_host(uint64_t g) { return (HANDLE)(uintptr_t)g; }
static int copy_file_new(const char *from, const char *to) { return CopyFileA(from, to, 1) != 0; }
#else
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/stat.h>
#define PSEP '/'
#define PSEP_S "/"
typedef int FsH;
#define FSH_NONE (-1)
static FsH fsh_open(const char *p, int write) { return open(p, (write ? O_RDWR : O_RDONLY) | O_CLOEXEC); }
static FsH fsh_create(const char *p) { return open(p, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0664); }
static void fsh_close(FsH h) { close(h); }
static uint64_t fsh_pread(FsH h, uint64_t pos, void *buf, uint64_t size) {
    uint64_t total = 0;
    while (total < size) {
        size_t chunk = size - total > 0x40000000 ? 0x40000000 : (size_t)(size - total);
        ssize_t got = pread(h, (char *)buf + total, chunk, (off_t)(pos + total));
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) break;
        total += (uint64_t)got;
    }
    return total;
}
static int fsh_pwrite(FsH h, uint64_t pos, const void *buf, uint32_t size) {
    uint32_t done = 0;
    while (done < size) {
        ssize_t put = pwrite(h, (const char *)buf + done, size - done, (off_t)(pos + done));
        if (put < 0 && errno == EINTR) continue;
        if (put <= 0) return 0;
        done += (uint32_t)put;
    }
    return 1;
}
static int fsh_size(FsH h, int64_t *out) { struct stat st; if (fstat(h, &st)) return 0; *out = (int64_t)st.st_size; return 1; }
static int fsh_truncate(FsH h, int64_t size) { return ftruncate(h, (off_t)size) == 0; }
static void fsh_flush(FsH h) { fsync(h); }
static uint64_t fsh_guest(FsH h) { return (uint64_t)(uint32_t)h + 0x100000000ull; }
static FsH fsh_host(uint64_t g) { return (int)(uint32_t)(g - 0x100000000ull); }
static int copy_file_new(const char *from, const char *to) {      /* fails if `to` exists, as CopyFile(.., TRUE) */
    int in = open(from, O_RDONLY | O_CLOEXEC), out = in < 0 ? -1 : open(to, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0664);
    char buf[1 << 16];
    ssize_t n;
    int ok = out >= 0;
    while (ok && (n = read(in, buf, sizeof buf)) > 0) ok = write(out, buf, (size_t)n) == n;
    if (in >= 0) close(in);
    if (out >= 0) { if (ok) fsync(out); close(out); }
    return ok;
}
#endif

/* nn::fs results (module 2), value = description << 9 | 2. Descriptions as published (switchbrew "Error codes",
 * the same numbers the suyu reference uses in file_sys/errors.h): 1 PathNotFound, 2 PathAlreadyExists,
 * 1002 TargetNotFound (no such save data), 3005 OutOfRange (read past the end of the file),
 * 6201 FileExtensionWithoutOpenModeAllowAppend (write past the end of a file opened without the append mode),
 * 6202 ReadNotPermitted, 6203 WriteNotPermitted (the handle was opened without that mode).
 * Read in sdk.elf: 60 MountNameAlreadyExists, 6905 NotMounted [0x1f498c], 6457 "a file is still open for writing" [0x1f44f4].
 * 7 DirectoryNotEmpty, 6400 PermissionDenied (suyu file_sys/errors.h; write to the read-only rom). */
#define RESULT_PATH_NOT_FOUND 0x202u
#define RESULT_PATH_ALREADY_EXISTS 0x402u
#define RESULT_DIRECTORY_NOT_EMPTY 0xe02u
#define RESULT_MOUNT_NAME_EXISTS 0x7802u
#define RESULT_TARGET_NOT_FOUND 0x7d402u
#define RESULT_OUT_OF_RANGE 0x177a02u
#define RESULT_NO_APPEND 0x307202u
#define RESULT_READ_NOT_PERMITTED 0x307402u
#define RESULT_WRITE_NOT_PERMITTED 0x307602u
#define RESULT_PERMISSION_DENIED 0x320002u
#define RESULT_WRITE_FILE_OPEN 0x327202u
#define RESULT_NOT_MOUNTED 0x35f202u
#define RESULT_GENERIC_FAILURE 0x3e802u      /* a Windows failure with no console counterpart (disk error, sharing violation) */

enum { MK_ROM = 1, MK_SAVE, MK_CACHE, MK_PLAIN /* self-tests: a writable host folder without a journal */ };
typedef struct {
    char name[16];
    int kind;
    char root[PLAT_MAX_PATH];                     /* committed state (or the rom folder) */
    char work[PLAT_MAX_PATH];                     /* journal working folder ("" for the rom) */
    char **tomb;                             /* relative paths deleted since the last commit, PSEP separators */
    int tomb_n;
} Mount;
#define MOUNT_MAX 8
static Mount g_mounts[MOUNT_MAX];
static char g_rom_root[PLAT_MAX_PATH], g_user_root[PLAT_MAX_PATH];
static volatile int32_t g_open_log;
static plat_mutex g_fs_lock;
static volatile int32_t g_fs_lock_ready;

static void fs_lock(void) {
    if (plat_atomic_cas(&g_fs_lock_ready, 1, 0) == 0) { plat_mutex_init(&g_fs_lock); g_fs_lock_ready = 2; }
    while (g_fs_lock_ready != 2) plat_yield();
    plat_mutex_lock(&g_fs_lock);
}
static void fs_unlock(void) { plat_mutex_unlock(&g_fs_lock); }

static void fs_roots(void) {
    if (g_rom_root[0]) return;
    const char *env = getenv("SMO_ROMFS"), *user = getenv("SMO_USERDATA");
    strncpy(g_rom_root, env && env[0] ? env : ".." PSEP_S "romfs", PLAT_MAX_PATH - 1);
    strncpy(g_user_root, user && user[0] ? user : ".." PSEP_S "userdata", PLAT_MAX_PATH - 1);
    plat_mkdirs(g_user_root);
}
const char *fs_user_root(void) { fs_roots(); return g_user_root; }
const char *fs_rom_root(void) { fs_roots(); return g_rom_root; }           /* smo_ui.c: the game's own files for the host menus */

static int is_dir(const char *p) { return plat_is_dir(p); }
static int exists(const char *p) { return plat_exists(p); }
static void make_parents(const char *path) {             /* creates every missing folder above `path` */
    char tmp[PLAT_MAX_PATH];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 3; *p; p++) if (*p == PSEP) { *p = 0; plat_mkdirs(tmp); *p = PSEP; }
}
static void remove_tree(const char *dir) {
    char sub[PLAT_MAX_PATH];
    plat_dir *d = plat_dir_open(dir);
    const char *name;
    int isd;
    while (d && plat_dir_next(d, &name, &isd, NULL)) {
        snprintf(sub, sizeof sub, "%s" PSEP_S "%s", dir, name);
        if (isd) remove_tree(sub);
        else {
#ifdef _WIN32
            SetFileAttributesA(sub, FILE_ATTRIBUTE_NORMAL);
#endif
            plat_remove(sub);
        }
    }
    plat_dir_close(d);
    plat_remove(dir);
}

/* ---- tombstones */
static int tomb_find(Mount *m, const char *rel) {
    for (int i = 0; i < m->tomb_n; i++) if (plat_stricmp(m->tomb[i], rel) == 0) return i;
    return -1;
}
static int tomb_hides(Mount *m, const char *rel) {       /* rel itself or one of its parent folders was deleted */
    for (int i = 0; i < m->tomb_n; i++) {
        size_t n = strlen(m->tomb[i]);
        if (plat_strnicmp(m->tomb[i], rel, n) == 0 && (rel[n] == 0 || rel[n] == PSEP)) return 1;
    }
    return 0;
}
static void tomb_add(Mount *m, const char *rel) {
    if (tomb_find(m, rel) >= 0) return;
    m->tomb = realloc(m->tomb, sizeof(char *) * (size_t)(m->tomb_n + 1));
    m->tomb[m->tomb_n++] = strdup(rel);
}
static void tomb_remove(Mount *m, const char *rel) {
    int i = tomb_find(m, rel);
    if (i >= 0) { free(m->tomb[i]); m->tomb[i] = m->tomb[--m->tomb_n]; }
}
static void tomb_clear(Mount *m) { for (int i = 0; i < m->tomb_n; i++) free(m->tomb[i]); free(m->tomb); m->tomb = NULL; m->tomb_n = 0; }

/* ---- path resolution */
static Mount *mount_find(const char *name, size_t n) {
    for (int i = 0; i < MOUNT_MAX; i++) if (g_mounts[i].kind && strlen(g_mounts[i].name) == n && strncmp(g_mounts[i].name, name, n) == 0) return &g_mounts[i];
    return NULL;
}
/* "mount:/dir/file" -> mount and relative path with PSEP separators (no leading or trailing separator). Result code. */
static uint32_t split_path(const char *guest, Mount **mount, char *rel) {
    const char *colon = guest ? strchr(guest, ':') : NULL;
    if (!colon) return RESULT_NOT_MOUNTED;
    fs_roots();
    Mount *m = mount_find(guest, (size_t)(colon - guest));
    if (!m) return RESULT_NOT_MOUNTED;
    const char *rest = colon + 1;
    while (*rest == '/' || *rest == PSEP) rest++;
    snprintf(rel, PLAT_MAX_PATH, "%s", rest);
    for (char *p = rel; *p; p++) if (*p == '/') *p = PSEP;
    size_t len = strlen(rel);
    while (len > 0 && rel[len - 1] == PSEP) rel[--len] = 0;
    *mount = m;
    return 0;
}
static void join(char *out, const char *root, const char *rel) { if (rel[0]) snprintf(out, PLAT_MAX_PATH, "%s" PSEP_S "%s", root, rel); else snprintf(out, PLAT_MAX_PATH, "%s", root); }
/* Android port, MODS: <data root>/mods/<name>/ holds files that replace the game's own (as <name>/romfs/<path> or
 * <name>/<path>); config/mods.ini switches one off with "<name> = off" (the launcher writes it). Enabled mods are
 * looked at in alphabetical order before the game's romfs: the first that has the file wins. A mod replaces files the
 * game has; a file only a mod has is found when the game opens it by name, not in a directory listing. */
#define MOD_MAX 32
static char g_mod_dir[MOD_MAX][PLAT_MAX_PATH];
static int g_mod_n = -1;
static int mod_cmp(const void *a, const void *b) { return strcmp((const char *)a, (const char *)b); }
static void mods_load(void) {                            /* fs lock held */
    if (g_mod_n >= 0) return;
    g_mod_n = 0;
    char base[PLAT_MAX_PATH], ini[PLAT_MAX_PATH], names[MOD_MAX][128];
    int n = 0;
    snprintf(base, sizeof base, "%s" PSEP_S "mods", plat_data_root());
    plat_dir *d = plat_dir_open(base);
    const char *name; int dir; int64_t size;
    while (d && n < MOD_MAX && plat_dir_next(d, &name, &dir, &size)) if (dir && name[0] != '.') snprintf(names[n++], sizeof names[0], "%s", name);
    if (d) plat_dir_close(d);
    qsort(names, (size_t)n, sizeof names[0], mod_cmp);
    snprintf(ini, sizeof ini, "%s" PSEP_S "config" PSEP_S "mods.ini", plat_data_root());
    for (int i = 0; i < n; i++) {
        int on = 1;
        FILE *f = fopen(ini, "r");
        char line[256], key[128], val[32];
        while (f && fgets(line, sizeof line, f))
            if (line[0] != ';' && sscanf(line, " %127[^=] = %31s", key, val) == 2) {
                size_t k = strlen(key);
                while (k && (key[k - 1] == ' ' || key[k - 1] == '\t')) key[--k] = 0;
                if (!strcmp(key, names[i])) on = strcmp(val, "off") != 0;
            }
        if (f) fclose(f);
        printf("fs: mod \"%s\" %s\n", names[i], on ? "on" : "off (mods.ini)");
        if (!on) continue;
        char romfs[PLAT_MAX_PATH];
        snprintf(romfs, sizeof romfs, "%s" PSEP_S "%s" PSEP_S "romfs", base, names[i]);
        if (is_dir(romfs)) snprintf(g_mod_dir[g_mod_n++], PLAT_MAX_PATH, "%s", romfs);
        else snprintf(g_mod_dir[g_mod_n++], PLAT_MAX_PATH, "%s" PSEP_S "%s", base, names[i]);
    }
}
/* The host path that currently holds `rel` in the mount's view; returns 0 when it does not exist in the view. */
static int view_path(Mount *m, const char *rel, char *out) {
    if (m->kind == MK_ROM && rel[0]) {                   /* mods first */
        mods_load();
        for (int i = 0; i < g_mod_n; i++) { join(out, g_mod_dir[i], rel); if (exists(out) && !is_dir(out)) return 1; }
    }
    if (m->work[0]) {
        if (tomb_hides(m, rel)) { join(out, m->work, rel); return exists(out); }      /* re-created after a deletion: only the working copy counts */
        join(out, m->work, rel);
        if (exists(out)) return 1;
    }
    join(out, m->root, rel);
    return exists(out);
}
static int view_parent_is_dir(Mount *m, const char *rel) {
    char parent[PLAT_MAX_PATH], host[PLAT_MAX_PATH];
    snprintf(parent, sizeof parent, "%s", rel);
    char *s = strrchr(parent, PSEP);
    if (!s) return 1;                                     /* directly under the root */
    *s = 0;
    return view_path(m, parent, host) && is_dir(host);
}
/* SMO port (nnmovie_mf.c): the host file that holds guest path "mount:/dir/file" in its mount's view; 0 if none. On the
 * console the movie library's Extractor::SetDataSource(const char*) opens such a path through nn::fs itself. */
int fs_host_path(const char *guest, char *out, size_t n) {
    Mount *m; char rel[PLAT_MAX_PATH], host[PLAT_MAX_PATH];
    int ok = 0;
    fs_lock();
    if (!split_path(guest, &m, rel) && view_path(m, rel, host) && !is_dir(host)) { snprintf(out, n, "%s", host); ok = 1; }
    fs_unlock();
    return ok;
}
/* Used by nnfsdir.c: the host folders to list for a guest directory (working folder first), 0 = no such directory. */
int fs_dir_roots(const char *guest, char roots[2][PLAT_MAX_PATH], uint32_t *result) {
    Mount *m; char rel[PLAT_MAX_PATH], host[PLAT_MAX_PATH];
    int n = 0;
    fs_lock();
    *result = split_path(guest, &m, rel);
    if (!*result) {
        if (!view_path(m, rel, host) || !is_dir(host)) *result = RESULT_PATH_NOT_FOUND;
        else {
            if (m->work[0]) { join(roots[n], m->work, rel); if (is_dir(roots[n])) n++; }
            if (!m->work[0] || !tomb_hides(m, rel)) { join(roots[n], m->root, rel); if (is_dir(roots[n])) n++; }
        }
    }
    fs_unlock();
    return n;
}
int fs_dir_hidden(const char *guest_dir, const char *name) {      /* 1 = this entry of the committed folder was deleted in the working state */
    Mount *m; char rel[PLAT_MAX_PATH], full[PLAT_MAX_PATH];
    int hidden = 0;
    fs_lock();
    if (!split_path(guest_dir, &m, rel) && m->work[0]) { join(full, rel, name); hidden = tomb_hides(m, rel[0] ? full : name); }
    fs_unlock();
    return hidden;
}
uint32_t fs_delete_directory(const char *guest) {
    Mount *m; char rel[PLAT_MAX_PATH], host[PLAT_MAX_PATH], roots[2][PLAT_MAX_PATH];
    uint32_t r;
    fs_lock();
    r = split_path(guest, &m, rel);
    if (!r && m->kind == MK_ROM) r = RESULT_PERMISSION_DENIED;
    if (!r && (!view_path(m, rel, host) || !is_dir(host))) r = RESULT_PATH_NOT_FOUND;
    fs_unlock();
    if (r) return r;
    uint32_t r2;                                          /* empty in the view? */
    int n = fs_dir_roots(guest, roots, &r2);
    for (int i = 0; i < n; i++) {
        plat_dir *d = plat_dir_open(roots[i]);
        const char *name;
        while (d && plat_dir_next(d, &name, NULL, NULL)) {
            int from_committed = !m->work[0] || plat_strnicmp(roots[i], m->work, strlen(m->work)) != 0;
            if (from_committed && m->work[0] && fs_dir_hidden(guest, name)) continue;       /* deleted in the working state */
            plat_dir_close(d);
            return RESULT_DIRECTORY_NOT_EMPTY;
        }
        plat_dir_close(d);
    }
    fs_lock();
    if (m->work[0]) {
        char w[PLAT_MAX_PATH], cm[PLAT_MAX_PATH];
        join(w, m->work, rel); join(cm, m->root, rel);
        if (is_dir(w)) plat_remove(w);
        if (is_dir(cm)) tomb_add(m, rel);
    } else r = plat_remove(host) ? 0 : RESULT_GENERIC_FAILURE;
    fs_unlock();
    return r;
}

/* Open files: nn::fs::FileHandle is the Windows handle itself; the open mode (1 read, 2 write, 4 allow append) and the
 * mount are kept here so reads, writes, growth and Commit behave exactly as the mode says. */
#define OPEN_MAX 1024
static struct { FsH h; int used, mode; Mount *mount; } g_open[OPEN_MAX];
static void open_remember(FsH h, int mode, Mount *m) {
    fs_lock();
    for (int i = 0; i < OPEN_MAX; i++) if (!g_open[i].used) { g_open[i].h = h; g_open[i].used = 1; g_open[i].mode = mode; g_open[i].mount = m; break; }
    fs_unlock();
}
static int open_mode(FsH h, int forget) {                /* mode of an open handle, 7 (everything) if unknown */
    int mode = 7;
    fs_lock();
    for (int i = 0; i < OPEN_MAX; i++) if (g_open[i].used && g_open[i].h == h) { mode = g_open[i].mode; if (forget) g_open[i].used = 0; break; }
    fs_unlock();
    return mode;
}

/* ---- journal: commit and recovery */
#ifdef _WIN32
typedef HANDLE CommitLock;
static CommitLock commit_mutex(void) {
    HANDLE mx = CreateMutexA(NULL, 0, "smo_native_save_commit");
    if (mx) WaitForSingleObject(mx, PLAT_INFINITE);
    return mx;
}
static void commit_done(CommitLock mx) { if (mx) { ReleaseMutex(mx); CloseHandle(mx); } }
#else
typedef int CommitLock;                               /* every process that commits takes <userdata>/.commit.lock */
static CommitLock commit_mutex(void) {
    char path[PLAT_MAX_PATH];
    snprintf(path, sizeof path, "%s/.commit.lock", g_user_root);
    int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0664);
    if (fd >= 0) while (flock(fd, LOCK_EX) != 0 && errno == EINTR) {}
    return fd;
}
static void commit_done(CommitLock fd) { if (fd >= 0) { flock(fd, LOCK_UN); close(fd); } }
#endif
/* Leaving the game (Android: GameActivity's Exit, android_main.c): waits for a commit in progress and for any file
 * operation, then keeps new ones from starting. The process ends next, so neither lock is given back; the committed
 * save is whole, and changes the game had not committed are dropped, as on the console when it is switched off. */
void fs_quiesce(void) {
    fs_lock();                                            /* fs_Commit holds it through the whole commit */
    (void)commit_mutex();                                 /* and the launcher's save import (another process) */
}
static void journal_list(FILE *j, const char *work, const char *rel) {     /* every file of the working folder, depth first */
    char dir[PLAT_MAX_PATH], sub[PLAT_MAX_PATH];
    join(dir, work, rel);
    plat_dir *d = plat_dir_open(dir);
    const char *name;
    int isd;
    while (d && plat_dir_next(d, &name, &isd, NULL)) {
        if (rel[0]) snprintf(sub, sizeof sub, "%s" PSEP_S "%s", rel, name); else snprintf(sub, sizeof sub, "%s", name);
        if (isd) { fprintf(j, "M\t%s\n", sub); journal_list(j, work, sub); }
        else {
            char full[PLAT_MAX_PATH];
            join(full, work, sub);
            FsH f = fsh_open(full, 1);
            if (f != FSH_NONE) { fsh_flush(f); fsh_close(f); }
            fprintf(j, "F\t%s\n", sub);
        }
    }
    plat_dir_close(d);
}
/* Applies a decided commit: marker lines are "W <working folder>", "R <committed folder>", then "D rel" (delete),
 * "M rel" (make folder), "F rel" (move file). Every step can be repeated, so a commit interrupted here is finished by
 * running it again. */
static int journal_apply(const char *marker) {
    char line[PLAT_MAX_PATH + 8], work[PLAT_MAX_PATH] = "", root[PLAT_MAX_PATH] = "", a[PLAT_MAX_PATH], b[PLAT_MAX_PATH];
    FILE *j = fopen(marker, "r");
    int n = 0;
    if (!j) return -1;
    while (fgets(line, sizeof line, j)) {
        size_t len = strlen(line);
        while (len && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = 0;
        if (len < 3) continue;
        const char *arg = line + 2;
        if (line[0] == 'W') snprintf(work, sizeof work, "%s", arg);
        else if (line[0] == 'R') snprintf(root, sizeof root, "%s", arg);
        else if (!root[0] || !work[0]) continue;
        else if (line[0] == 'D') { join(a, root, arg); if (is_dir(a)) remove_tree(a); else plat_remove(a); n++; }
        else if (line[0] == 'M') { join(a, root, arg); make_parents(a); plat_mkdirs(a); }
        else if (line[0] == 'F') {
            join(a, work, arg); join(b, root, arg);
            if (exists(a)) { make_parents(b); if (!plat_rename(a, b)) { fclose(j); return -2; } }
            n++;
        }
    }
    fclose(j);
    plat_remove(marker);
    return n;
}
static int pid_alive(uint32_t pid) {
#ifdef _WIN32
    HANDLE p = OpenProcess(SYNCHRONIZE, 0, pid);
    if (!p) return GetLastError() == ERROR_ACCESS_DENIED;
    int alive = WaitForSingleObject(p, 0) == WAIT_TIMEOUT;
    CloseHandle(p);
    return alive;
#else
    if (pid > 0x7fffffffu) return 0;
    return kill((pid_t)pid, 0) == 0 || errno == EPERM;
#endif
}
/* Before a save-type folder is mounted: finish a commit that was decided but not applied, and drop the working
 * folders and half-written journals of processes that no longer run. */
static void journal_recover(const char *root) {
    char marker[PLAT_MAX_PATH], path[PLAT_MAX_PATH], parent[PLAT_MAX_PATH];
    CommitLock mx = commit_mutex();
    snprintf(marker, sizeof marker, "%s.commit", root);
    if (exists(marker)) {
        int n = journal_apply(marker);
        printf("nn::fs: a commit of %s was interrupted after it was decided: finished now (%d changes)\n", root, n);
        trace_printf("     fs journal: interrupted commit of %s rolled forward (%d changes)\n", root, n);
    }
    {   /* entries "<root name>.work.<pid>" / "<root name>.journal.<pid>" beside the root */
        snprintf(parent, sizeof parent, "%s", root);
        char *cut = strrchr(parent, PSEP);
        const char *leaf = cut ? cut + 1 : parent;
        char leafbuf[PLAT_MAX_PATH];
        snprintf(leafbuf, sizeof leafbuf, "%s", leaf);
        if (cut) *cut = 0; else snprintf(parent, sizeof parent, ".");
        size_t ll = strlen(leafbuf);
        plat_dir *d = plat_dir_open(parent);
        const char *name;
        int isd;
        while (d && plat_dir_next(d, &name, &isd, NULL)) {
            if (plat_strnicmp(name, leafbuf, ll) || name[ll] != '.') continue;
            const char *ext = strrchr(name, '.');
            const char *kind = ext && ext - 5 >= name && !strncmp(ext - 5, ".work", 5) ? "work" : ext && ext - 8 >= name && !strncmp(ext - 8, ".journal", 8) ? "journal" : NULL;
            if (!kind) continue;
            uint32_t pid = (uint32_t)strtoul(ext + 1, NULL, 10);
            if (!pid || pid == plat_pid() || pid_alive(pid)) continue;
            snprintf(path, sizeof path, "%s" PSEP_S "%s", parent, name);
            if (isd) remove_tree(path); else plat_remove(path);
            trace_printf("     fs journal: uncommitted %s of process %lu discarded (%s)\n", kind, (unsigned long)pid, name);
        }
        plat_dir_close(d);
    }
    commit_done(mx);
}
static uint32_t mount_add(const char *name, int kind, const char *root) {
    Mount *m = NULL;
    size_t n = name ? strlen(name) : 0;
    if (!n || n >= sizeof m->name) return RESULT_GENERIC_FAILURE;
    fs_lock();
    if (mount_find(name, n)) { fs_unlock(); return RESULT_MOUNT_NAME_EXISTS; }
    for (int i = 0; i < MOUNT_MAX && !m; i++) if (!g_mounts[i].kind) m = &g_mounts[i];
    if (!m) { fs_unlock(); return RESULT_GENERIC_FAILURE; }
    memset(m, 0, sizeof *m);
    strcpy(m->name, name);
    snprintf(m->root, PLAT_MAX_PATH, "%s", root);
    if (kind != MK_ROM) {
        snprintf(m->work, PLAT_MAX_PATH, "%s.work.%lu", root, (unsigned long)plat_pid());
        remove_tree(m->work);                              /* a second mount in this process starts from the committed state */
        plat_mkdirs(m->work);
    }
    m->kind = kind;
    fs_unlock();
    return 0;
}

static void save_dir(char *dir, const char *leaf) { fs_roots(); snprintf(dir, PLAT_MAX_PATH, "%s" PSEP_S "%s", g_user_root, leaf); }
/* Save data of the one local user lives in <userdata>\save. It exists once EnsureSaveData has created it;
 * MountSaveData of save data that does not exist fails as on the console. */
static int save_exists(void) { char dir[PLAT_MAX_PATH]; save_dir(dir, "save"); return is_dir(dir); }
static void fs_MountRom(Ctx *c) {                        /* (name, cache buffer, cache size): the buffer holds the RomFS metadata cache of the SDK's reader; the host reads the extracted tree */
    fs_roots();
    uint32_t r = mount_add(STR(0), MK_ROM, g_rom_root);
    trace_printf("     fs mount rom \"%s\" -> %s (%x)\n", STR(0), g_rom_root, r);
    RET(r);
}
static void fs_MountSaveData(Ctx *c) {                   /* (const char *name, const Uid &) */
    char dir[PLAT_MAX_PATH];
    save_dir(dir, "save");
    if (!save_exists()) { trace_printf("     fs mount save data \"%s\": no save data (EnsureSaveData was not called)\n", STR(0)); RET(RESULT_TARGET_NOT_FOUND); return; }
    journal_recover(dir);
    uint32_t r = mount_add(STR(0), MK_SAVE, dir);
    trace_printf("     fs mount save data \"%s\" -> %s, journal working folder %s.work.%lu (%x)\n", STR(0), dir, dir, (unsigned long)plat_pid(), r);
    RET(r);
}
static void fs_MountCacheStorage(Ctx *c) {               /* (const char *name): the application's cache storage, created on first use */
    char dir[PLAT_MAX_PATH];
    save_dir(dir, "cache");
    plat_mkdirs(dir);
    journal_recover(dir);
    uint32_t r = mount_add(STR(0), MK_CACHE, dir);
    trace_printf("     fs mount cache storage \"%s\" -> %s (%x)\n", STR(0), dir, r);
    RET(r);
}
static void fs_Unmount(Ctx *c) {                         /* (const char *name): uncommitted changes are dropped */
    const char *name = STR(0);
    fs_lock();
    Mount *m = name ? mount_find(name, strlen(name)) : NULL;
    if (m) {
        for (int i = 0; i < OPEN_MAX; i++) if (g_open[i].used && g_open[i].mount == m) { fsh_close(g_open[i].h); g_open[i].used = 0; }   /* [sdk 0x1f4870] the accessor closes what is left open */
        if (m->work[0]) remove_tree(m->work);
        tomb_clear(m);
        trace_printf("     fs unmount \"%s\"\n", name);
        m->kind = 0;
    } else trace_printf("     fs unmount \"%s\": not mounted\n", name ? name : "(null)");
    fs_unlock();
}
static void fs_EnsureSaveData(Ctx *c) {                  /* (const Uid &): creates the user's save data if there is none */
    char dir[PLAT_MAX_PATH];
    save_dir(dir, "save");
    RET(plat_mkdirs(dir) ? 0 : RESULT_GENERIC_FAILURE);
}
static void fs_IsSaveDataExisting(Ctx *c) { RET(save_exists()); }
static void fs_Commit(Ctx *c) {                          /* (const char *mount name) */
    const char *name = STR(0);
    char journal[PLAT_MAX_PATH], marker[PLAT_MAX_PATH];
    fs_lock();
    Mount *m = name ? mount_find(name, strlen(name)) : NULL;
    if (!m) { fs_unlock(); RET(RESULT_NOT_MOUNTED); return; }
    if (m->kind == MK_ROM) { fs_unlock(); RET(0); return; }                 /* nothing to commit on a read-only file system */
    for (int i = 0; i < OPEN_MAX; i++)
        if (g_open[i].used && g_open[i].mount == m && (g_open[i].mode & 2)) { fs_unlock(); trace_printf("     fs commit \"%s\": refused, a file is open for writing\n", name); RET(RESULT_WRITE_FILE_OPEN); return; }
    CommitLock mx = commit_mutex();
    snprintf(journal, sizeof journal, "%s.journal.%lu", m->root, (unsigned long)plat_pid());
    snprintf(marker, sizeof marker, "%s.commit", m->root);
    FILE *j = fopen(journal, "w");
    int ok = j != NULL, changes = -1;
    if (ok) {
        fprintf(j, "W\t%s\nR\t%s\n", m->work, m->root);
        for (int i = 0; i < m->tomb_n; i++) fprintf(j, "D\t%s\n", m->tomb[i]);
        journal_list(j, m->work, "");
        ok = fflush(j) == 0 && plat_fsync(j);
        fclose(j);
    }
    if (ok) ok = plat_rename(journal, marker);      /* the commit is decided here */
    if (ok) { changes = journal_apply(marker); ok = changes >= 0; }
    if (ok) tomb_clear(m);
    commit_done(mx);
    fs_unlock();
    trace_printf("     fs commit \"%s\": %s (%d changes)\n", name, ok ? "ok" : "FAILED", changes);
    if (!ok) printf("nn::fs::Commit(\"%s\") failed on the host; the committed save data is unchanged or will be completed at the next mount\n", name);
    RET(ok ? 0 : RESULT_GENERIC_FAILURE);
}
static void fs_ok(Ctx *c) { RET(0); }

/* [sdk 0x2175e0] QueryMountRomCacheSize asks the RomFS reader for the working memory its metadata needs: the sizes of the
 * four tables of the RomFS image (directory hash table, directory entries, file hash table, file entries). The image
 * itself is not on this machine (only the extracted tree is), so the tables are rebuilt from the tree with the format's
 * rules: a directory entry is 0x18 bytes + its name padded to 4, a file entry 0x20 + name padded to 4, a hash table has
 * 4 bytes per bucket and the bucket count the RomFS builder derives from the entry count. NOT VERIFIED against the
 * image's own header (it would need the original romfs image). */
static uint32_t romfs_buckets(uint32_t n) {
    if (n < 3) return 3;
    if (n < 19) return n | 1;
    while (n % 2 == 0 || n % 3 == 0 || n % 5 == 0 || n % 7 == 0 || n % 11 == 0 || n % 13 == 0 || n % 17 == 0) n++;
    return n;
}
static void romfs_walk(const char *dir, uint32_t *dirs, uint32_t *files, uint64_t *dir_bytes, uint64_t *file_bytes) {
#ifndef _WIN32
    char sub[PLAT_MAX_PATH];
    plat_dir *d = plat_dir_open(dir);              /* names are UTF-8 here already */
    const char *name;
    int isd;
    while (d && plat_dir_next(d, &name, &isd, NULL)) {
        int len = (int)strlen(name);
        if (isd) {
            (*dirs)++; *dir_bytes += 0x18 + (uint64_t)((len + 3) & ~3);
            snprintf(sub, sizeof sub, "%s/%s", dir, name);
            romfs_walk(sub, dirs, files, dir_bytes, file_bytes);
        } else { (*files)++; *file_bytes += 0x20 + (uint64_t)((len + 3) & ~3); }
    }
    plat_dir_close(d);
#else
    wchar_t wpat[PLAT_MAX_PATH + 4];
    char pat[PLAT_MAX_PATH + 4], sub[PLAT_MAX_PATH], utf8[1024];
    WIN32_FIND_DATAW fd;
    snprintf(pat, sizeof pat, "%s" PSEP_S "*", dir);
    MultiByteToWideChar(CP_ACP, 0, pat, -1, wpat, PLAT_MAX_PATH + 4);
    HANDLE h = FindFirstFileW(wpat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        int len = WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, utf8, sizeof utf8, NULL, NULL) - 1;
        if (len < 0) len = 0;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            (*dirs)++; *dir_bytes += 0x18 + (uint64_t)((len + 3) & ~3);
            WideCharToMultiByte(CP_ACP, 0, fd.cFileName, -1, utf8, sizeof utf8, NULL, NULL);
            snprintf(sub, sizeof sub, "%s" PSEP_S "%s", dir, utf8);
            romfs_walk(sub, dirs, files, dir_bytes, file_bytes);
        } else { (*files)++; *file_bytes += 0x20 + (uint64_t)((len + 3) & ~3); }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
#endif
}
static void fs_QueryMountRomCacheSize(Ctx *c) {          /* (size_t *out) */
    uint32_t dirs = 1, files = 0;
    uint64_t dir_bytes = 0x18, file_bytes = 0;
    fs_roots();
    romfs_walk(g_rom_root, &dirs, &files, &dir_bytes, &file_bytes);
    uint64_t size = 4ull * romfs_buckets(dirs) + dir_bytes + 4ull * romfs_buckets(files) + file_bytes;
    trace_printf("     fs QueryMountRomCacheSize: %u directories, %u files -> %llu bytes\n", dirs, files, (unsigned long long)size);
    wr64(A0, size);
    RET(0);
}

static void fs_OpenFile(Ctx *c) {                     /* (FileHandle *out, const char *path, int mode) */
    char rel[PLAT_MAX_PATH], path[PLAT_MAX_PATH];
    Mount *m = NULL;
    FsH h = FSH_NONE;
    int write = (A2 & 2) != 0;
    fs_lock();
    uint32_t r = split_path(STR(1), &m, rel);
    if (!r && write && m->kind == MK_ROM) r = RESULT_PERMISSION_DENIED;
    if (!r) {
        int found = view_path(m, rel, path) && !is_dir(path);
        if (!found) r = RESULT_PATH_NOT_FOUND;
        else if (write && m->work[0]) {                    /* copy-on-write: the committed file is never opened for writing */
            char w[PLAT_MAX_PATH];
            join(w, m->work, rel);
            if (plat_stricmp(w, path) != 0) { make_parents(w); if (!copy_file_new(path, w)) r = RESULT_GENERIC_FAILURE; }
            strcpy(path, w);
        }
    }
    fs_unlock();
    if (!r) {
        h = fsh_open(path, write);
        if (h == FSH_NONE) r = RESULT_PATH_NOT_FOUND;
    }
    if (plat_atomic_inc(&g_open_log) <= 200 || r)     /* D2: every failed open is logged, not only the first 200 */
        trace_printf("     fs open \"%s\" mode %d -> %s\n", STR(1), (int)A2, r == RESULT_NOT_MOUNTED ? "NOT MOUNTED" : r ? "NOT FOUND" : "ok");
    {   static int on = -1; if (on < 0) on = getenv("SMO_FSLOG") != NULL;       /* D2 debug: every open on stdout */
        if (on) printf("fslog: open \"%s\" mode %d -> %s\n", STR(1), (int)A2, r ? "NOT FOUND" : "ok"); }
    if (r) { wr64(A0, 0); RET(r); return; }
    open_remember(h, (int)(A2 & 7), m);
    wr64(A0, fsh_guest(h));
    RET(0);
}
static void fs_CloseFile(Ctx *c) { if (A0) { open_mode(fsh_host(A0), 1); fsh_close(fsh_host(A0)); } }
extern int g_hitch_ms; void hitch_note(const char *s);      /* hitch.c: SMO_HITCH */
static uint64_t read_at(FsH h, int64_t offset, void *buf, uint64_t size) {
    if (g_hitch_ms) { char note[80]; snprintf(note, sizeof note, "read of %llu KB", (unsigned long long)(size >> 10)); hitch_note(note); }
    uint64_t total = fsh_pread(h, (uint64_t)offset, buf, size);
    if (g_hitch_ms) hitch_note(NULL);
    return total;
}
static volatile int32_t g_read_log;
static void fs_ReadFile(Ctx *c) {                     /* (handle, offset, buffer, size) */
    uint64_t got = read_at(fsh_host(A0), (int64_t)A1, (void *)(uintptr_t)A2, A3);
    if (plat_atomic_inc(&g_read_log) <= 60)
        trace_printf("     fs read offset %llx size %llx -> %llx bytes  from=%llx\n", (unsigned long long)A1,
                     (unsigned long long)A3, (unsigned long long)got, (unsigned long long)c->x[30] - 4);
    if (got != A3 && getenv("SMO_FSLOG")) printf("fslog: ReadFile SHORT handle %llx offset %llx size %llx got %llx from=%llx\n", (unsigned long long)A0, (unsigned long long)A1, (unsigned long long)A3, (unsigned long long)got, (unsigned long long)c->x[30] - 4);   /* D2 debug */
    RET(got == A3 ? 0 : RESULT_OUT_OF_RANGE);            /* this overload must read exactly `size` bytes */
}
static void fs_ReadFileCount(Ctx *c) {                /* (size_t *outRead, handle, offset, buffer, size) */
    uint64_t got = read_at(fsh_host(A1), (int64_t)A2, (void *)(uintptr_t)A3, A4);
    wr64(A0, got);
    if (got != A4 && getenv("SMO_FSLOG")) printf("fslog: ReadFile(count) SHORT handle %llx offset %llx size %llx got %llx from=%llx\n", (unsigned long long)A1, (unsigned long long)A2, (unsigned long long)A4, (unsigned long long)got, (unsigned long long)c->x[30] - 4);   /* D2 debug */
    RET(0);
}
static void fs_WriteFile(Ctx *c) {                    /* (handle, offset, buffer, size, option) */
    FsH h = fsh_host(A0);
    int mode = open_mode(h, 0);
    int64_t size;
    if (!(mode & 2)) { RET(RESULT_WRITE_NOT_PERMITTED); return; }
    if (!(mode & 4) && fsh_size(h, &size) && (int64_t)(A1 + A3) > size) { RET(RESULT_NO_APPEND); return; }   /* a file only grows in append mode */
    if (A3 && !fsh_pwrite(h, A1, (const void *)(uintptr_t)A2, (uint32_t)A3)) { RET(RESULT_GENERIC_FAILURE); return; }
    if (A4 && (rd32(A4) & 1)) fsh_flush(h);                                  /* WriteOption::Flush */
    RET(0);
}
static void fs_GetFileSize(Ctx *c) {                  /* (s64 *out, handle) */
    int64_t size;
    if (!fsh_size(fsh_host(A1), &size)) { RET(RESULT_GENERIC_FAILURE); return; }
    wr64(A0, (uint64_t)size);
    RET(0);
}
/* SMO port: SetFileSize (handle, size): the file grows (zero-filled) or is cut to `size`; only on a handle opened for
 * writing (on a journaled mount that is the working copy, committed with the rest). sead::NinFileDeviceBase::doOpen_
 * calls it when it opens a file to write it new. FlushFile (handle): what was written reaches the disk. */
static void fs_SetFileSize(Ctx *c) {
    if (!(open_mode(fsh_host(A0), 0) & 2)) { RET(RESULT_WRITE_NOT_PERMITTED); return; }
    if ((int64_t)A1 < 0) { RET(RESULT_OUT_OF_RANGE); return; }
    if (!fsh_truncate(fsh_host(A0), (int64_t)A1)) { RET(RESULT_GENERIC_FAILURE); return; }
    RET(0);
}
/* nnlibc2_c5b.inc (musl's O_TRUNC): the file behind a guest FileHandle gets this size; 1 on success */
int fs_host_set_size(uint64_t guest, int64_t size) { return fsh_truncate(fsh_host(guest), size); }
static void fs_FlushFile(Ctx *c) { if (open_mode(fsh_host(A0), 0) & 2) fsh_flush(fsh_host(A0)); RET(0); }
/* Where a new entry goes: the working folder of a journaled mount. Result code; `path` = host path to create. */
static uint32_t create_target(const char *guest, Mount **mount, char *rel, char *path) {
    char host[PLAT_MAX_PATH];
    uint32_t r = split_path(guest, mount, rel);
    if (r) return r;
    if ((*mount)->kind == MK_ROM) return RESULT_PERMISSION_DENIED;
    if (!rel[0] || view_path(*mount, rel, host)) return RESULT_PATH_ALREADY_EXISTS;
    if (!view_parent_is_dir(*mount, rel)) return RESULT_PATH_NOT_FOUND;
    join(path, (*mount)->work, rel);
    make_parents(path);
    return 0;
}
static void fs_CreateFile(Ctx *c) {                   /* (path, size) */
    char rel[PLAT_MAX_PATH], path[PLAT_MAX_PATH];
    Mount *m;
    fs_lock();
    uint32_t r = create_target(STR(0), &m, rel, path);
    if (!r) {
        FsH h = fsh_create(path);
        if (h == FSH_NONE) r = RESULT_GENERIC_FAILURE;
        else {
            fsh_truncate(h, (int64_t)A1);
            fsh_close(h);
            /* the deletion of an older file of this name stays recorded: the commit deletes it, then moves the new one in */
        }
    }
    fs_unlock();
    RET(r);
}
static void fs_DeleteFile(Ctx *c) {
    char rel[PLAT_MAX_PATH], path[PLAT_MAX_PATH], w[PLAT_MAX_PATH], cm[PLAT_MAX_PATH];
    Mount *m;
    fs_lock();
    uint32_t r = split_path(STR(0), &m, rel);
    if (!r && m->kind == MK_ROM) r = RESULT_PERMISSION_DENIED;
    if (!r && (!view_path(m, rel, path) || is_dir(path))) r = RESULT_PATH_NOT_FOUND;
    if (!r) {
        join(w, m->work, rel); join(cm, m->root, rel);
        if (exists(w)) plat_remove(w);
        if (exists(cm)) tomb_add(m, rel);
    }
    fs_unlock();
    RET(r);
}
static void fs_CreateDirectory(Ctx *c) {
    char rel[PLAT_MAX_PATH], path[PLAT_MAX_PATH];
    Mount *m;
    fs_lock();
    uint32_t r = create_target(STR(0), &m, rel, path);
    if (!r && !plat_mkdirs(path)) r = RESULT_GENERIC_FAILURE;
    fs_unlock();
    RET(r);
}
static void fs_GetEntryType(Ctx *c) {                 /* (DirectoryEntryType *out, path): 0 directory, 1 file */
    char rel[PLAT_MAX_PATH], path[PLAT_MAX_PATH];
    Mount *m;
    fs_lock();
    uint32_t r = split_path(STR(1), &m, rel);
    if (!r && !view_path(m, rel, path)) r = RESULT_PATH_NOT_FOUND;
    fs_unlock();
    {   static int on = -1; if (on < 0) on = getenv("SMO_FSLOG") != NULL;       /* SMO debug: every entry-type query on stdout */
        if (on) printf("fslog: entry type \"%s\" -> %s\n", STR(1), r ? "NOT FOUND" : is_dir(path) ? "directory" : "file"); }
    if (r) { RET(r); return; }
    wr32(A0, is_dir(path) ? 0 : 1);
    RET(0);
}
static void fs_OpenDirectory(Ctx *c) { wr64(A0, 0); RET(RESULT_PATH_NOT_FOUND); }   /* replaced by nnfsdir.c */
static void fs_false(Ctx *c) { RET(0); }

/* ---- self-test of the mount table and of the save data journal, in a scratch folder under %TEMP% (SMO_SDK_SELFTEST) */
uint32_t fs_mount_plain(const char *name, const char *host_dir) { fs_roots(); return mount_add(name, MK_PLAIN, host_dir); }
static int st_host_is(const char *path, const char *text) {
    char buf[64] = "";
    FILE *f = fopen(path, "rb");
    if (!f) return text == NULL;
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    return text && strcmp(buf, text) == 0;
}
static uint32_t st_write(Ctx *c, const char *guest, const char *text, uint64_t *keep_open) {
    uint64_t h = 0;
    uint32_t option = 1;
    c->x[0] = (uint64_t)(uintptr_t)&h; c->x[1] = (uint64_t)(uintptr_t)guest; c->x[2] = 7; fs_OpenFile(c);
    if (c->x[0]) return (uint32_t)c->x[0];
    c->x[0] = h; c->x[1] = 0; c->x[2] = (uint64_t)(uintptr_t)text; c->x[3] = strlen(text); c->x[4] = (uint64_t)(uintptr_t)&option; fs_WriteFile(c);
    uint32_t r = (uint32_t)c->x[0];
    if (keep_open) *keep_open = h; else { c->x[0] = h; fs_CloseFile(c); }
    return r;
}
static int st_read_is(Ctx *c, const char *guest, const char *text) {
    uint64_t h = 0, got = 0;
    char buf[64] = "";
    c->x[0] = (uint64_t)(uintptr_t)&h; c->x[1] = (uint64_t)(uintptr_t)guest; c->x[2] = 1; fs_OpenFile(c);
    if (c->x[0]) return text == NULL;
    c->x[0] = (uint64_t)(uintptr_t)&got; c->x[1] = h; c->x[2] = 0; c->x[3] = (uint64_t)(uintptr_t)buf; c->x[4] = sizeof buf - 1; fs_ReadFileCount(c);
    c->x[0] = h; fs_CloseFile(c);
    buf[got] = 0;
    return text && strcmp(buf, text) == 0;
}
int nnfs_selftest(Ctx *c) {
    char saved_root[PLAT_MAX_PATH], t[PLAT_MAX_PATH], save[PLAT_MAX_PATH], a[PLAT_MAX_PATH], n[PLAT_MAX_PATH], work[PLAT_MAX_PATH], marker[PLAT_MAX_PATH], journal[PLAT_MAX_PATH];
    int bad = 0;
    uint64_t uid[2] = {1, 0}, h = 0;
    #define FSCHECK(cond, text) do { if (!(cond)) { printf("  FAIL fs journal: %s\n", text); bad++; } } while (0)
    fs_roots();
    strcpy(saved_root, g_user_root);
    snprintf(t, PLAT_MAX_PATH - 64, "%s" PSEP_S, plat_temp_dir());
    snprintf(t + strlen(t), 64, "smo_fs_selftest_%lu", (unsigned long)plat_pid());
    remove_tree(t);
    plat_mkdirs(t);
    strcpy(g_user_root, t);
    snprintf(save, sizeof save, "%s" PSEP_S "save", t); plat_mkdirs(save);
    snprintf(a, sizeof a, "%s" PSEP_S "a.bin", save); snprintf(n, sizeof n, "%s" PSEP_S "sub" PSEP_S "new.bin", save);
    snprintf(work, sizeof work, "%s.work.%lu", save, (unsigned long)plat_pid());
    snprintf(marker, sizeof marker, "%s.commit", save); snprintf(journal, sizeof journal, "%s.journal.%lu", save, (unsigned long)plat_pid());
    FILE *f = fopen(a, "wb"); if (f) { fputs("committed", f); fclose(f); }

    c->x[0] = (uint64_t)(uintptr_t)"nosuch:/a.bin"; c->x[1] = 0; fs_CreateFile(c);
    FSCHECK(c->x[0] == RESULT_NOT_MOUNTED, "a path with an unmounted name is not NotMounted");
    c->x[0] = (uint64_t)(uintptr_t)"st"; c->x[1] = (uint64_t)(uintptr_t)uid; fs_MountSaveData(c);
    FSCHECK(c->x[0] == 0, "MountSaveData failed");
    c->x[0] = (uint64_t)(uintptr_t)"st"; c->x[1] = (uint64_t)(uintptr_t)uid; fs_MountSaveData(c);
    FSCHECK(c->x[0] == RESULT_MOUNT_NAME_EXISTS, "mounting the same name twice is not MountNameAlreadyExists");
    /* 1. a write is visible to the game and NOT in the committed folder */
    FSCHECK(st_write(c, "st:/a.bin", "CHANGED!!", NULL) == 0, "write failed");
    FSCHECK(st_read_is(c, "st:/a.bin", "CHANGED!!"), "the game does not read back what it wrote");
    FSCHECK(st_host_is(a, "committed"), "an uncommitted write reached the committed folder");
    c->x[0] = (uint64_t)(uintptr_t)"st:/sub"; fs_CreateDirectory(c);
    c->x[0] = (uint64_t)(uintptr_t)"st:/sub/new.bin"; c->x[1] = 0; fs_CreateFile(c);
    FSCHECK(c->x[0] == 0 && !exists(n), "a new file went straight to the committed folder");
    /* 2. unmount without commit: everything is rolled back */
    c->x[0] = (uint64_t)(uintptr_t)"st"; fs_Unmount(c);
    FSCHECK(!exists(work), "the working folder survived Unmount");
    c->x[0] = (uint64_t)(uintptr_t)"st"; c->x[1] = (uint64_t)(uintptr_t)uid; fs_MountSaveData(c);
    FSCHECK(st_read_is(c, "st:/a.bin", "committed") && st_read_is(c, "st:/sub/new.bin", NULL), "uncommitted changes survived a remount");
    /* 3. commit is refused while a file is open for writing, then applies everything */
    FSCHECK(st_write(c, "st:/a.bin", "SECOND!!!", &h) == 0, "write failed");
    c->x[0] = (uint64_t)(uintptr_t)"st"; fs_Commit(c);
    FSCHECK(c->x[0] == RESULT_WRITE_FILE_OPEN && st_host_is(a, "committed"), "Commit with a file open for writing was not refused");
    c->x[0] = h; fs_CloseFile(c);
    c->x[0] = (uint64_t)(uintptr_t)"st:/sub"; fs_CreateDirectory(c);
    c->x[0] = (uint64_t)(uintptr_t)"st:/sub/new.bin"; c->x[1] = 0; fs_CreateFile(c);
    FSCHECK(st_write(c, "st:/sub/new.bin", "", NULL) == 0, "open of a created file failed");
    c->x[0] = (uint64_t)(uintptr_t)"st"; fs_Commit(c);
    FSCHECK(c->x[0] == 0 && st_host_is(a, "SECOND!!!") && exists(n) && !exists(marker), "Commit did not apply the changes");
    FSCHECK(st_read_is(c, "st:/a.bin", "SECOND!!!"), "the committed file is not read back after Commit");
    /* 4. deletion: hidden at once, done on the host at Commit */
    c->x[0] = (uint64_t)(uintptr_t)"st:/sub/new.bin"; fs_DeleteFile(c);
    FSCHECK(c->x[0] == 0 && exists(n) && st_read_is(c, "st:/sub/new.bin", NULL), "DeleteFile is not journaled");
    c->x[0] = (uint64_t)(uintptr_t)"st:/sub/new.bin"; fs_DeleteFile(c);
    FSCHECK(c->x[0] == RESULT_PATH_NOT_FOUND, "deleting a deleted file is not PathNotFound");
    c->x[0] = (uint64_t)(uintptr_t)"st"; fs_Commit(c);
    FSCHECK(c->x[0] == 0 && !exists(n), "a committed deletion did not reach the host");
    /* 5. a process killed after the commit was decided: the next mount finishes it */
    FSCHECK(st_write(c, "st:/a.bin", "THIRD!!!!", NULL) == 0, "write failed");
    f = fopen(journal, "w");
    if (f) { fprintf(f, "W\t%s\nR\t%s\n", work, save); journal_list(f, work, ""); fclose(f); }
    plat_rename(journal, marker);
    FSCHECK(st_host_is(a, "SECOND!!!") && exists(marker), "test setup: decided commit");
    journal_recover(save);
    FSCHECK(st_host_is(a, "THIRD!!!!") && !exists(marker), "a decided commit was not rolled forward at the next mount");
    /* 6. a process killed before the commit was decided: its working folder is dropped (pid 4 is the System process: use a dead pid) */
    {
        char dead[PLAT_MAX_PATH];
        snprintf(dead, sizeof dead, "%s.work.4294967294", save);
        plat_mkdirs(dead);
        snprintf(dead + strlen(dead), 32, "" PSEP_S "a.bin");
        f = fopen(dead, "wb"); if (f) { fputs("LOST", f); fclose(f); }
        journal_recover(save);
        dead[strlen(dead) - 6] = 0;
        FSCHECK(!exists(dead) && st_host_is(a, "THIRD!!!!"), "the working folder of a dead process was not discarded");
    }
    c->x[0] = (uint64_t)(uintptr_t)"st"; fs_Unmount(c);
    remove_tree(t);
    strcpy(g_user_root, saved_root);
    printf("  fs: mount table (NotMounted, MountNameAlreadyExists) and save data journal (copy-on-write, rollback at Unmount, Commit refused with a\n"
           "      writer open, two-phase Commit, journaled deletion, roll-forward of a decided commit, discard of a dead process's work): %s\n", bad ? "FAILED" : "ok");
    #undef FSCHECK
    return bad;
}

/* ---- nn::account: one local user with a fixed id */
static void acc_ListAllUsers(Ctx *c) { wr32(A0, 1); if ((int)A2 > 0) { wr64(A1, 1); wr64(A1 + 8, 0); } RET(0); }
static volatile int32_t g_acc_user_open;                    /* the one local user {1, 0} has been opened (there is no CloseUser import) */
int account_user_is_open(uint64_t lo, uint64_t hi) { return g_acc_user_open && lo == 1 && hi == 0; }
static void acc_TryOpenPreselectedUser(Ctx *c) { wr64(A0, 1); wr64(A0 + 8, 0); wr64(A0 + 16, 0); plat_atomic_xchg(&g_acc_user_open, 1); RET(1); }
static void acc_GetUserId(Ctx *c) { wr64(A0, 1); wr64(A0 + 8, 0); RET(0); }
/* SMO port (SDK 3.5.1 forms the game imports). [doc] OpenPreselectedUser(UserHandle*): the user chosen at start-up,
 * opened (the SDK aborts if there is none; here there always is the one local user {1, 0}). OpenUser(UserHandle*, Uid&):
 * opens that user; any other uid is not a user of this console (account 124-0006... is NOT VERIFIED, so it stops).
 * CloseUser(UserHandle&) closes it. GetLastOpenedUser(Uid*): the user opened most recently on the console = {1, 0}. */
static void acc_OpenPreselectedUser(Ctx *c) { acc_TryOpenPreselectedUser(c); }
static void acc_OpenUser(Ctx *c) {
    if (rd64(A1) != 1 || rd64(A1 + 8) != 0) guest_trap("nn::account::OpenUser: not the console's user", c->x[30] - 4);
    wr64(A0, 1); wr64(A0 + 8, 0); wr64(A0 + 16, 0); plat_atomic_xchg(&g_acc_user_open, 1); RET(0);
}
static void acc_CloseUser(Ctx *c) { (void)c; plat_atomic_xchg(&g_acc_user_open, 0); }
static void acc_GetLastOpenedUser(Ctx *c) { wr64(A0, 1); wr64(A0 + 8, 0); RET(0); }
/* The nickname of the console user is the name of the Windows user (SMO_NICKNAME overrides): UTF-8, at most 32
 * bytes, NUL-terminated, in the 33-byte Nickname. */
static void acc_GetNickname(Ctx *c) {
    static char name[33];
    static volatile int32_t ready;
    if (plat_atomic_cas(&ready, 1, 0) == 0) {
        const char *env = getenv("SMO_NICKNAME");
        char utf8[512] = "";
        if (env && env[0]) snprintf(utf8, sizeof utf8, "%s", env);
        else {
#ifdef _WIN32
            wchar_t wname[257] = L"";
            uint32_t n = 257;
            if (GetUserNameW(wname, (DWORD *)&n)) WideCharToMultiByte(CP_UTF8, 0, wname, -1, utf8, sizeof utf8, NULL, NULL);
#endif
        }
        if (!utf8[0]) strcpy(utf8, "Player");
        size_t len = strlen(utf8);
        if (len > 32) { len = 32; while (len > 0 && ((unsigned char)utf8[len] & 0xc0) == 0x80) len--; }   /* cut between characters */
        memcpy(name, utf8, len);
        name[len] = 0;
        ready = 2;
    }
    while (ready != 2) plat_yield();
    memset((void *)(uintptr_t)A0, 0, 33);
    strcpy((char *)(uintptr_t)A0, name);
    RET(0);
}
static void acc_out_true(Ctx *c) { wr8(A0, 1); RET(0); }
static void acc_out_false(Ctx *c) { wr8(A0, 0); RET(0); }
/* [sdk] 0x4b3a8 (EnsureNetworkServiceAccountAvailable): the SDK treats the account results 124-0200 .. 124-0269 from
 * the service as "no usable network service account" (it then offers the account-linking applet and returns the
 * result). An offline machine has no linked account: 124-0200 = 0x1907c, the first code of that range, is returned
 * (NOT VERIFIED: which code of the range a console gives; the linking applet is not shown, it needs the network). */
#define RESULT_ACCOUNT_NSA_UNAVAILABLE 0x1907cull
static void acc_unavailable(Ctx *c) { RET(RESULT_ACCOUNT_NSA_UNAVAILABLE); }
static void keep(Ctx *c) { (void)c; }

/* ---- nn::nifm: no network */
static void nifm_false(Ctx *c) { RET(0); }

#define FS(n) "_ZN2nn2fs" n
#define AC(n) "_ZN2nn7account" n
#define NI(n) "_ZN2nn4nifm" n
static const ImportDef fs_imports[] = {
    {FS("8MountRomEPKcPvm"), fs_MountRom}, {FS("13MountSaveDataEPKcRKNS_7account3UidE"), fs_MountSaveData}, {FS("17MountCacheStorageEPKc"), fs_MountCacheStorage},
    {FS("7UnmountEPKc"), fs_Unmount}, {FS("22QueryMountRomCacheSizeEPm"), fs_QueryMountRomCacheSize}, {FS("6CommitEPKc"), fs_Commit}, {FS("14CommitSaveDataEPKc"), fs_Commit} /* SMO: the 3.5.1 name; [sdk 0x4e72c] detail::Find + FileSystemAccessor::Commit, as Commit */, {FS("14EnsureSaveDataERKNS_7account3UidE"), fs_EnsureSaveData},
    {FS("18IsSaveDataExistingERKNS_7account3UidE"), fs_IsSaveDataExisting},
    {FS("8OpenFileEPNS0_10FileHandleEPKci"), fs_OpenFile}, {FS("9CloseFileENS0_10FileHandleE"), fs_CloseFile},
    {FS("8ReadFileENS0_10FileHandleElPvm"), fs_ReadFile}, {FS("8ReadFileEPmNS0_10FileHandleElPvm"), fs_ReadFileCount},
    {FS("9WriteFileENS0_10FileHandleElPKvmRKNS0_11WriteOptionE"), fs_WriteFile}, {FS("11GetFileSizeEPlNS0_10FileHandleE"), fs_GetFileSize},
    {FS("11SetFileSizeENS0_10FileHandleEl"), fs_SetFileSize}, {FS("9FlushFileENS0_10FileHandleE"), fs_FlushFile},
    {FS("10CreateFileEPKcl"), fs_CreateFile}, {FS("10DeleteFileEPKc"), fs_DeleteFile}, {FS("15CreateDirectoryEPKc"), fs_CreateDirectory},
    {FS("12GetEntryTypeEPNS0_18DirectoryEntryTypeEPKc"), fs_GetEntryType}, {FS("13OpenDirectoryEPNS0_15DirectoryHandleEPKci"), fs_OpenDirectory},
    {AC("10InitializeEv"), fs_ok}, {AC("12ListAllUsersEPiPNS0_3UidEi"), acc_ListAllUsers},
    {AC("22TryOpenPreselectedUserEPNS0_10UserHandleE"), acc_TryOpenPreselectedUser},
    {AC("19OpenPreselectedUserEPNS0_10UserHandleE"), acc_OpenPreselectedUser}, {AC("8OpenUserEPNS0_10UserHandleERKNS0_3UidE"), acc_OpenUser},
    {AC("9CloseUserERKNS0_10UserHandleE"), acc_CloseUser}, {AC("17GetLastOpenedUserEPNS0_3UidE"), acc_GetLastOpenedUser},
    {FS("8ReadFileEPmNS0_10FileHandleElPvmRKNS0_10ReadOptionE"), fs_ReadFileCount},   /* SMO port: ReadOption (x5) has no flags in this SDK */
    {AC("9GetUserIdEPNS0_3UidERKNS0_10UserHandleE"), acc_GetUserId}, {AC("11GetNicknameEPNS0_8NicknameERKNS0_3UidE"), acc_GetNickname},
    {AC("16GetUserExistenceEPbRKNS0_3UidE"), acc_out_true},
    {AC("32IsNetworkServiceAccountAvailableEPbRKNS0_10UserHandleE"), acc_out_false},
    {AC("36EnsureNetworkServiceAccountAvailableERKNS0_10UserHandleE"), acc_unavailable},
    {AC("25CachedNintendoAccountInfoC1Ev"), keep}, {AC("12AsyncContextC1Ev"), keep}, {AC("12AsyncContextD1Ev"), keep},
    {AC("12AsyncContext7HasDoneEPb"), acc_out_true}, {AC("12AsyncContext9GetResultEv"), acc_unavailable},
    {NI("10InitializeEv"), fs_ok}, {NI("18IsNetworkAvailableEv"), nifm_false}, {NI("22IsNetworkRequestOnHoldEv"), nifm_false},
    {NI("20SubmitNetworkRequestEv"), keep}, {NI("27SubmitNetworkRequestAndWaitEv"), keep}, {NI("20CancelNetworkRequestEv"), keep},
};

void register_fs_imports(void) {
    register_imports(fs_imports, (int)(sizeof fs_imports / sizeof fs_imports[0]));
}
