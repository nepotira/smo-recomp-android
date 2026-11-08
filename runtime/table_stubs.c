/* table_stubs.c: Dynamic bridge between libmain.so (our compiled C runtime) and libsmo.so
 * (the pre-lifted 74,822 game functions, 1,336 lifted SDK functions, and symbol tables).
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <dlfcn.h>
#include <sys/mman.h>
#include <unistd.h>
#include "runtime.h"
#include "hostrt.h"
#include "sdklift.h"

#undef fn_table
#undef fn_table_count
#undef dyn_syms
#undef dyn_sym_count
#undef sdk_data
#undef sdk_data_count
#undef sdk_fns
#undef sdk_fn_count
#undef sdk_host_names
#undef sdk_host_name_count
#undef sdk_ctors
#undef sdk_ctor_count

static const FnEntry k_empty_fn[1] = {{0, 0}};
static const int k_zero = 0;
static const DynSym k_empty_sym[1] = {{"", 0, 0}};
static const SdkData k_empty_sdk_data[1] = {{0, 0, 0}};
static const SdkFn k_empty_sdk_fn[1] = {{0, 0, 0}};
static const char *const k_empty_sdk_host[1] = {0};
static const uint64_t k_empty_sdk_ctor[1] = {0};

const FnEntry (*game_fn_table_p)[] = &k_empty_fn;
const int *game_fn_table_count_p = &k_zero;
const DynSym (*game_dyn_syms_p)[] = &k_empty_sym;
const int *game_dyn_sym_count_p = &k_zero;

const SdkData (*game_sdk_data_p)[] = &k_empty_sdk_data;
const int *game_sdk_data_count_p = &k_zero;
const SdkFn (*game_sdk_fns_p)[] = &k_empty_sdk_fn;
const int *game_sdk_fn_count_p = &k_zero;
const char *const (*game_sdk_host_names_p)[] = &k_empty_sdk_host;
const int *game_sdk_host_name_count_p = &k_zero;
const uint64_t (*game_sdk_ctors_p)[] = &k_empty_sdk_ctor;
const int *game_sdk_ctor_count_p = &k_zero;

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
    uintptr_t ps = (uintptr_t)sysconf(_SC_PAGESIZE);
    if (!ps) ps = 4096;
    uintptr_t page_start = p & ~(ps - 1);
    uintptr_t page_end = (p + 16 + ps - 1) & ~(ps - 1);
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
    const SdkData *sd = (const SdkData *)dlsym(g_libsmo_handle, "sdk_data");
    const int *sdc = (const int *)dlsym(g_libsmo_handle, "sdk_data_count");
    if (sd && sdc) {
        game_sdk_data_p = (const SdkData (*)[])sd;
        game_sdk_data_count_p = sdc;
    }
    const SdkFn *sf = (const SdkFn *)dlsym(g_libsmo_handle, "sdk_fns");
    const int *sfc = (const int *)dlsym(g_libsmo_handle, "sdk_fn_count");
    const char *const *shn = (const char *const *)dlsym(g_libsmo_handle, "sdk_host_names");
    const int *shnc = (const int *)dlsym(g_libsmo_handle, "sdk_host_name_count");
    const uint64_t *sct = (const uint64_t *)dlsym(g_libsmo_handle, "sdk_ctors");
    const int *sctc = (const int *)dlsym(g_libsmo_handle, "sdk_ctor_count");
    if (sf && sfc && shn && shnc && sct && sctc) {
        game_sdk_fns_p = (const SdkFn (*)[])sf;
        game_sdk_fn_count_p = sfc;
        game_sdk_host_names_p = (const char *const (*)[])shn;
        game_sdk_host_name_count_p = shnc;
        game_sdk_ctors_p = (const uint64_t (*)[])sct;
        game_sdk_ctor_count_p = sctc;
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
        {"sdk_host_call", (void *)sdk_host_call},
        {"sdk_trap_call", (void *)sdk_trap_call},
        {"sdk_dispatch", (void *)sdk_dispatch},
        {"sdk_jmp_record", (void *)sdk_jmp_record},
        {"sdk_jmp_restore", (void *)sdk_jmp_restore},
        {"sdk_longjmp", (void *)sdk_longjmp},
        {"__wrap_f_400072e61c", (void *)__wrap_f_400072e61c},
        {"__wrap_f_4000731fdc", (void *)__wrap_f_4000731fdc},
        {"__wrap_f_4000a58394", (void *)__wrap_f_4000a58394},
    };
    int patched = 0;
    for (unsigned i = 0; i < sizeof(hooks) / sizeof(hooks[0]); i++) {
        void *sym = dlsym(g_libsmo_handle, hooks[i].name);
        if (smo_patch_arm64_jump(sym, hooks[i].dst)) patched++;
    }
    fprintf(stderr, "smo: [NEPO-RUNTIME-ACTIVE] libmain.so bound to libsmo.so (%d fns, %d syms, %d sdk_fns, %d bridges)\n",
            ftc ? *ftc : 0, dsc ? *dsc : 0, sfc ? *sfc : 0, patched);
}
