/* Host runtime interface for the fully lifted game. */
#ifndef RECOMP_HOSTRT_H
#define RECOMP_HOSTRT_H

#include <stdio.h>
#include <stdarg.h>
#include "runtime.h"

#include "image.h"               /* generated per build: tools/lifter/gen_tables.py */
/* Android port: the guest ranges come with the generated tables (SMO_LAYOUT, tools/target.py); the console's own
 * addresses when the tables are older than the layouts */
#ifdef SMO_LAYOUT_GUEST_BASE
#define GUEST_BASE SMO_LAYOUT_GUEST_BASE
#else
#define GUEST_BASE 0x7100000000ULL
#endif
/* SMO port: code that used a Super Smash Bros. Ultimate 13.0.4 address for a Smash-only diagnostic or test and has no
 * SMO equivalent yet is disabled through this constant (never used for game behaviour). */
#define SMO_UNPORTED 0
#ifdef SMO_LAYOUT_IMPORT_BASE
#define IMPORT_BASE SMO_LAYOUT_IMPORT_BASE   /* placeholder addresses for symbols of other modules */
#else
#define IMPORT_BASE 0x7300000000ULL      /* placeholder addresses for symbols of other modules */
#endif
#define IMPORT_SLOT 256ULL

typedef void (*ImportFn)(Ctx *c);
typedef struct { const char *name; ImportFn fn; } ImportDef;

#ifdef SMO_LAYOUT_HOSTFN_BASE
#define HOSTFN_BASE SMO_LAYOUT_HOSTFN_BASE   /* placeholder addresses for host functions handed out at run time */
#else
#define HOSTFN_BASE 0x7400000000ULL      /* placeholder addresses for host functions handed out at run time */
#endif

uint64_t hostfn_address(const char *name, ImportFn fn);
const char *hostfn_current_name(void);
unsigned long long hostfn_current_calls(void);
void print_hostfn_summary(FILE *out);
void report_and_exit(int code);
void runtime_init(const char *elf, const char *trace_path);
void *import_data(const char *name);
/* Make an imported data symbol resolve to host memory (call before runtime_init). */
void import_set_address(const char *name, void *addr);
typedef struct { const char *name; const unsigned char *bytes; unsigned size; } SdkData;
extern const SdkData sdk_data[];
extern const int sdk_data_count;
void register_imports(const ImportDef *defs, int count);
ImportFn import_lookup(const char *name);    /* registered handler of an imported function, or NULL */
ImportFn handler_by_name(const char *name);  /* the handler registered under a name, imported or not, or NULL */
void call_guest(Ctx *c, uint64_t addr);
uint64_t guest_function_of(uint64_t host_rip);
void print_import_summary(FILE *out);
void trace_printf(const char *fmt, ...);

#endif
