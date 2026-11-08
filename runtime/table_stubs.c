/* table_stubs.c: Dynamic bridge between libmain.so (our compiled C runtime) and libsmo.so
 * (the pre-lifted 74,822 game functions and tables).
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <dlfcn.h>
#include <sys/mman.h>
#include <unistd.h>
#include "runtime.h"

#undef fn_table
#undef fn_table_count
#undef dyn_syms
#undef dyn_sym_count

static const FnEntry k_empty_fn[1] = {{0, 0}};
static const int k_zero = 0;
static const DynSym k_empty_sym[1] = {{"", 0, 0}};

const FnEntry (*game_fn_table_p)[] = &k_empty_fn;
const int *game_fn_table_count_p = &k_zero;
const DynSym (*game_dyn_syms_p)[] = &k_empty_sym;
const int *game_dyn_sym_count_p = &k_zero;

static void (*g_real_72e61c)(Ctx *);
static void (*g_real_731fdc)(Ctx *);
static void (*g_real_a58394)(Ctx *);
static void *g_libsmo_handle;

void *smo_bridge_handle(void) { return g_libsmo_handle; }

void __real_f_400072e61c(Ctx *c) { if (g_real_72e61c) g_real_72e61c(c); }
void __real_f_4000731fdc(Ctx *c) { if (g_real_731fdc) g_real_731fdc(c); }
void __real_f_4000a58394(Ctx *c) { if (g_real_a58394) g_real_a58394(c); }

void __wrap_f_400072e61c(Ctx *c);
void __wrap_f_4000731fdc(Ctx *c);
void __wrap_f_4000a58394(Ctx *c);

int smo_patch_arm64_jump(void *src, void *dst) {
    if (!src || !dst || src == dst) return 0;
    uintptr_t p = (uintptr_t)src;
    uintptr_t page_start = p & ~(uintptr_t)0xfffULL;
    uintptr_t page_end = (p + 16 + 0xfffULL) & ~(uintptr_t)0xfffULL;
    if (mprotect((void *)page_start, page_end - page_start, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) return 0;
    uint32_t *ins = (uint32_t *)src;
    ins[0] = 0x58000050u; /* LDR X16, #8 */
    ins[1] = 0xd61f0200u; /* BR  X16     */
    uint64_t target = (uint64_t)(uintptr_t)dst;
    memcpy(&ins[2], &target, 8);
    __builtin___clear_cache((char *)src, (char *)src + 16);
    return 1;
}

__attribute__((constructor)) static void smo_bridge_init(void) {
    g_libsmo_handle = dlopen("libsmo.so", RTLD_NOW | RTLD_GLOBAL);
    if (!g_libsmo_handle) return;
    const FnEntry *ft = (const FnEntry *)dlsym(g_libsmo_handle, "fn_table");
    const int *ftc = (const int *)dlsym(g_libsmo_handle, "fn_table_count");
    const DynSym *ds = (const DynSym *)dlsym(g_libsmo_handle, "dyn_syms");
    const int *dsc = (const int *)dlsym(g_libsmo_handle, "dyn_sym_count");
    if (ft && ftc && ds && dsc) {
        game_fn_table_p = (const FnEntry (*)[])ft;
        game_fn_table_count_p = ftc;
        game_dyn_syms_p = (const DynSym (*)[])ds;
        game_dyn_sym_count_p = dsc;
    }
    g_real_72e61c = (void (*)(Ctx *))dlsym(g_libsmo_handle, "f_400072e61c");
    g_real_731fdc = (void (*)(Ctx *))dlsym(g_libsmo_handle, "f_4000731fdc");
    g_real_a58394 = (void (*)(Ctx *))dlsym(g_libsmo_handle, "f_4000a58394");

    struct { const char *name; void *dst; } hooks[] = {
        {"call_indirect", (void *)call_indirect},
        {"host_import", (void *)host_import},
        {"guest_trap", (void *)guest_trap},
        {"guest_unimpl", (void *)guest_unimpl},
        {"guest_svc", (void *)guest_svc},
        {"stx_diag", (void *)stx_diag},
        {"__wrap_f_400072e61c", (void *)__wrap_f_400072e61c},
        {"__wrap_f_4000731fdc", (void *)__wrap_f_4000731fdc},
        {"__wrap_f_4000a58394", (void *)__wrap_f_4000a58394},
    };
    int patched = 0;
    for (unsigned i = 0; i < sizeof(hooks) / sizeof(hooks[0]); i++) {
        void *sym = dlsym(g_libsmo_handle, hooks[i].name);
        if (smo_patch_arm64_jump(sym, hooks[i].dst)) patched++;
    }
    fprintf(stderr, "smo: [NEPO-RUNTIME-ACTIVE] libmain.so bound to libsmo.so (%d fns, %d syms, %d bridges)\n",
            ftc ? *ftc : 0, dsc ? *dsc : 0, patched);
}
