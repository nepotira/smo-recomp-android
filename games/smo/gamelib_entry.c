/* gamelib_entry.c: Dynamic entry point and dispatch tables for smogame.so */
#include <stdint.h>

#define SMOGAME_ABI 2
#ifndef SMOGAME_ID
#define SMOGAME_ID "smo-1.4.1-android-arm64"
#endif

typedef struct {
    uint32_t abi;
    const char *id;
    const uint64_t *main_addrs;
    const uint64_t *sdk_addrs;
    const void *main_ptrs;
    const void *sdk_ptrs;
    const uint64_t *main_count;
    const uint64_t *sdk_count;
} SmoGameTables;

extern const uint64_t g_smo_main_fn_addrs[];
extern const uint64_t g_smo_main_fn_count;
extern const void *g_smo_main_fn_ptrs[];

__attribute__((visibility("default")))
const SmoGameTables *smogame_tables(void) {
    static const SmoGameTables t = {
        SMOGAME_ABI,
        SMOGAME_ID,
        g_smo_main_fn_addrs,
        0,
        g_smo_main_fn_ptrs,
        0,
        &g_smo_main_fn_count,
        0
    };
    return &t;
}
