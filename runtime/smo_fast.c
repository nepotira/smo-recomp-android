/* smo_fast.c - SMO port: host versions of hot game code, put into the loaded game DLL at start-up.
 *
 * THE SAMPLER LIST SORT. agl::driver::NVNMgr::registerSampler_ [main] adds a sampler to the manager's list and then
 * sorts the WHOLE list again by its 16-bit key with sead::PtrArrayImpl::sort, a cocktail-shaker sort that calls the
 * comparator (agl::driver::NVNMgr::compareSamplerKey_) through a function pointer for every pair it looks at: at least
 * one pass over the list per new sampler. At Cascade Kingdom's first checkpoint the game registers about a dozen
 * samplers a frame (and releases as many): 85 million comparator calls in 30 s on the main thread (SMO_ICALLS), each
 * an indirect call into lifted code. That was 1-2 ms of the main thread a frame, and 8-10 ms in the frames where the
 * registrations bunch up, enough to miss the refresh (SMO_HITCH: the main thread inside compareSamplerKey_). Here the
 * call in registerSampler_ goes to the same algorithm in host code with the comparison inlined, so the list ends in
 * the same order, swap for swap; a call with any other comparator goes on to the lifted sort.
 *
 * HOW. Lifted functions call each other with direct calls (`f_<address>(c);`, compiled at -O1 without inlining to a
 * `call rel32` to the callee's entry, which ssbu-recomp's hook table relies on as well). The one call to the sort in
 * the lifted registerSampler_ is found inside that function's code (its range from the unwind table) by its target,
 * and its rel32 is pointed at a stub within 2 GB that jumps to the host function. Nothing else in the DLL changes, and
 * nothing at all when the call is not found exactly once (said in the log).
 *
 * SMO_FAST=0 leaves the game's code alone. SMO_FAST_CHECK=1 also runs the lifted sort on the same list and stops the
 * game if the two orders ever differ (counts at exit). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "plat.h"
#include "runtime.h"
#include "hostrt.h"

#define SYM_SORT GAME_SYM__ZN4sead12PtrArrayImpl4sortEPFiPKvS2_E
#define SYM_REGISTER GAME_SYM__ZN3agl6driver6NVNMgr16registerSampler_EiPK10NVNsamplerj
#define SYM_KEYCMP GAME_SYM__ZN3agl6driver6NVNMgr18compareSamplerKey_EPKNS1_10SamplerKeyES4_

static void (*g_lifted_sort)(Ctx *);
static int g_check;
static int64_t g_sorts, g_items, g_swaps, g_other, g_checked;

static void (*host_fn_of(uint64_t guest))(Ctx *) {          /* the lifted function at a guest address (fn_table is sorted) */
    int lo = 0, hi = fn_table_count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (fn_table[mid].addr == guest) return fn_table[mid].fn;
        if (fn_table[mid].addr < guest) lo = mid + 1; else hi = mid - 1;
    }
    return NULL;
}

/* compareSamplerKey_(a, b) [main]: the 16-bit key at offset 0x62 of a minus that of b */
static inline int key_cmp(uint64_t a, uint64_t b) { return (int)rd16(a + 0x62) - (int)rd16(b + 0x62); }

/* sead::PtrArrayImpl::sort [main], step for step: a forward pass over [lo, hi) swaps where cmp(p[k], p[k+1]) >= 1, a
 * backward pass from the forward pass's last swap down to lo swaps where cmp(p[k], p[k-1]) < 0; the next passes are
 * bounded by those last swaps; it ends when a pass's last swap is where it started. */
/* Android port: the shaker sort only ever swaps neighbours that are strictly out of order, so it is a stable sort, and
 * every stable sort by the same key ends in the same order: an insertion sort does, in one pass over a list that was
 * sorted before one sampler was added (the shaker sort went back and forth over it: 4% of the main thread). */
static int64_t insertion_sort_keys(uint64_t *p, int n) {
    int64_t moves = 0;
    for (int k = 1; k < n; k++) {
        uint64_t v = p[k];
        int key = rd16(v + 0x62), j = k;
        while (j > 0 && (int)rd16(p[j - 1] + 0x62) > key) { p[j] = p[j - 1]; j--; moves++; }
        p[j] = v;
    }
    return moves;
}
static int64_t shaker_sort_keys(uint64_t *p, int n) {
    int lo = 0, hi = n - 1;
    int64_t swaps = 0;
    for (;;) {
        int last = lo;
        for (int k = lo; k < hi; k++)
            if (key_cmp(p[k], p[k + 1]) >= 1) { uint64_t t = p[k]; p[k] = p[k + 1]; p[k + 1] = t; last = k; swaps++; }
        if (lo == last) return swaps;
        int first = last;
        for (int k = last; k > lo; k--)
            if (key_cmp(p[k], p[k - 1]) < 0) { uint64_t t = p[k]; p[k] = p[k - 1]; p[k - 1] = t; first = k; swaps++; }
        hi = last;
        if (first == last) return swaps;
        lo = first;
    }
}

/* Reached from the call in registerSampler_ (through the stub): sead::PtrArrayImpl::sort(this = x0, cmp = x1). The
 * registers the lifted sort would leave behind are only ones a callee may change (x0..x18, flags), and nothing the
 * caller reads after the call; the callee-saved ones are not touched. */
static void sampler_sort(Ctx *c) {
    if (c->x[1] != SYM_KEYCMP) { g_other++; g_lifted_sort(c); return; }
    int n = (int)rd32(c->x[0]);                  /* PtrArrayImpl: +0 count (int), +8 the pointers */
    if (n < 2) return;
    uint64_t *p = (uint64_t *)(uintptr_t)rd64(c->x[0] + 8);      /* guest addresses are host addresses */
    g_sorts++; g_items += n;
    {   /* the manager (the list is its member at +368): capacity +344, first ID +360, cursor +362, slots in use +408 */
        static int said, last_cur; uint64_t m = c->x[0] - 368;
        int cap = (int)rd32(m + 344), used = rd16(m + 408), cur = rd16(m + 362);
        if (n >= said + 64 || cur < last_cur) {
            printf("smo_fast: sampler pool: %d slots in the sorted list, %d in use of %d (first ID %d, cursor %d%s), %lld new slots so far\n", n, used, cap, rd16(m + 360), cur, cur < last_cur ? ": it wrapped" : "", (long long)g_sorts);
            if (n >= said + 64) said = n;
        }
        last_cur = cur;
    }
    if (!g_check) { g_swaps += insertion_sort_keys(p, n); return; }
    uint64_t *before = malloc((size_t)n * 8), *mine = malloc((size_t)n * 8);
    if (!before || !mine) guest_trap("smo_fast: out of host memory for SMO_FAST_CHECK", c->x[30]);
    memcpy(before, p, (size_t)n * 8);
    g_swaps += insertion_sort_keys(p, n);
    memcpy(mine, p, (size_t)n * 8);
    memcpy(p, before, (size_t)n * 8);
    g_lifted_sort(c);                            /* the game's own sort of the same list, as the call would have run */
    for (int i = 0; i < n; i++)
        if (p[i] != mine[i]) {
            printf("smo_fast: CHECK FAILED: sampler list sort #%lld of %d entries differs from the game's at index %d (%llx, game %llx)\n",
                   (long long)g_sorts, n, i, (unsigned long long)mine[i], (unsigned long long)p[i]);
            fflush(stdout);
            guest_trap("smo_fast: the host sampler list sort differs from the game's (SMO_FAST_CHECK)", c->x[30]);
        }
    g_checked++;
    free(before); free(mine);
}

#ifdef _WIN32
/* A page within 2 GB of `site` for the jump stub (a rel32 must reach it). */
static uint8_t *alloc_near(uint8_t *site) {
    uintptr_t base = (uintptr_t)site & ~(uintptr_t)0xffff;
    for (uintptr_t d = 0x10000; d < 0x7ff00000; d += 0x10000) {
        for (int s = 0; s < 2; s++) {
            uintptr_t a = s ? base + d : base - d;
            if (!s && base < d) continue;
            void *m = VirtualAlloc((void *)a, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
            if (m) return m;
        }
    }
    return NULL;
}

#endif

#ifndef _WIN32
/* Android port: code pages cannot be rewritten here, so the link does it: libsmo.so is linked with
 * -Wl,--wrap=f_400072e61c (tools/android/build_runtime.py), and every call of the lifted sead::PtrArrayImpl::sort from
 * another object file (the lifted callers, the function table call_indirect looks up) comes here. With the sampler key
 * comparator the host sort runs, otherwise the game's own (__real_). Measured on a Galaxy S24 Ultra in Cap Kingdom:
 * the sort and its comparator calls were about 20% of the game's main thread (the sort 10%, call_indirect 12%). */
void __real_f_400072e61c(Ctx *c);
static int g_android_fast;
void __wrap_f_400072e61c(Ctx *c) {
    if (g_android_fast && c->x[1] == SYM_KEYCMP) sampler_sort(c);
    else __real_f_400072e61c(c);
}
#endif

void smo_fast_install(void) {
#ifndef _WIN32
    const char *e = getenv("SMO_FAST");
    if (e && !atoi(e)) { printf("smo_fast: off (SMO_FAST=0): the game's sampler list sort runs as lifted\n"); return; }
    if (SYM_SORT != 0x400072e61cULL || !SYM_KEYCMP) {    /* the wrapped symbol is this version's sort in the android layout */
        printf("smo_fast: off: the sort is not where the link wrapped it (%llx); the game's sort runs as lifted\n", (unsigned long long)SYM_SORT);
        return;
    }
    g_check = getenv("SMO_FAST_CHECK") && atoi(getenv("SMO_FAST_CHECK"));
    g_lifted_sort = __real_f_400072e61c;
    g_android_fast = 1;
    printf("smo_fast: agl::driver::NVNMgr's sampler list is sorted in host code (wrapped at link time; SMO_FAST=0: the game's own code%s)\n",
           g_check ? "; SMO_FAST_CHECK=1: every sort compared with the game's" : ", SMO_FAST_CHECK=1: compare with it");
    return;
#else
    const char *e = getenv("SMO_FAST");
    if (e && !atoi(e)) { printf("smo_fast: off (SMO_FAST=0): the game's sampler list sort runs as lifted\n"); return; }
    if (!SYM_SORT || !SYM_REGISTER || !SYM_KEYCMP) {   /* gen_tables.py: not in this main.elf (a stripped update) */
        printf("smo_fast: off: this game version's sampler list functions are not known; its sort runs as lifted\n");
        return;
    }
    g_check = getenv("SMO_FAST_CHECK") && atoi(getenv("SMO_FAST_CHECK"));
    g_lifted_sort = host_fn_of(SYM_SORT);
    uint8_t *caller = (uint8_t *)(uintptr_t)host_fn_of(SYM_REGISTER);
    uint64_t img = 0;
    PRUNTIME_FUNCTION rf = caller ? RtlLookupFunctionEntry((uint64_t)(uintptr_t)caller, &img, NULL) : NULL;
    if (!g_lifted_sort || !rf || (uint8_t *)(uintptr_t)(img + rf->BeginAddress) != caller) {
        printf("smo_fast: NOT installed: the lifted registerSampler_ or sead::PtrArrayImpl::sort was not found, or has no unwind entry\n");
        return;
    }
    uint8_t *end = (uint8_t *)(uintptr_t)(img + rf->EndAddress), *at = NULL;
    int found = 0;
    for (uint8_t *q = caller; q + 5 <= end; q++) {        /* call rel32 whose target is the lifted sort */
        int32_t rel;
        memcpy(&rel, q + 1, 4);
        if (q[0] == 0xE8 && q + 5 + rel == (uint8_t *)(uintptr_t)g_lifted_sort) { at = q; found++; }
    }
    if (found != 1) { printf("smo_fast: NOT installed: %d calls of the sort in the lifted registerSampler_ (%d bytes of code), expected exactly 1\n", found, (int)(end - caller)); return; }
    uint8_t *stub = alloc_near(at);
    if (!stub) { printf("smo_fast: NOT installed: no page within 2 GB of the game's code for the jump\n"); return; }
    void (*target)(Ctx *) = sampler_sort;
    static const uint8_t jmp_abs[6] = {0xFF, 0x25, 0, 0, 0, 0};      /* jmp [rip + 0], then the 8-byte address */
    memcpy(stub, jmp_abs, 6); memcpy(stub + 6, &target, 8);
    uint32_t old;
    VirtualProtect(stub, 0x1000, PAGE_EXECUTE_READ, &old);
    int64_t rel64 = stub - (at + 5);
    if (rel64 != (int32_t)rel64) { printf("smo_fast: NOT installed: the stub is out of rel32 range\n"); return; }
    int32_t rel = (int32_t)rel64;
    if (!VirtualProtect(at + 1, 4, PAGE_EXECUTE_READWRITE, &old)) { printf("smo_fast: NOT installed: the game's code cannot be written (error %lu)\n", GetLastError()); return; }
    memcpy(at + 1, &rel, 4);
    VirtualProtect(at + 1, 4, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, 5);
    printf("smo_fast: agl::driver::NVNMgr::registerSampler_ sorts its sampler list in host code (SMO_FAST=0: the game's own code%s)\n",
           g_check ? "; SMO_FAST_CHECK=1: every sort compared with the game's" : ", SMO_FAST_CHECK=1: compare with it");
#endif
}

void smo_fast_report(FILE *out) {
    if (!g_sorts && !g_other) return;
    fprintf(out, "smo_fast: sampler list sorts in host code: %lld (%lld entries in all, %lld swaps); calls with another comparator, passed to the game's sort: %lld\n",
            (long long)g_sorts, (long long)g_items, (long long)g_swaps, (long long)g_other);
    if (g_check) fprintf(out, "smo_fast: SMO_FAST_CHECK: %lld sorts compared with the game's sort of the same list, all equal\n", (long long)g_checked);
}
