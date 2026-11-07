/* table_stubs.c: Default weak symbol definitions for lifted tables and wrapped functions.
 * When the lifted game module (smogame.so or lifted objects) is linked, its strong
 * definitions override these weak stubs seamlessly.
 */
#include "runtime.h"

__attribute__((weak)) const FnEntry fn_table[1] = {{0, 0}};
__attribute__((weak)) const int fn_table_count = 0;
__attribute__((weak)) const DynSym dyn_syms[1] = {{"", 0, 0}};
__attribute__((weak)) const int dyn_sym_count = 0;

__attribute__((weak)) void __real_f_400072e61c(Ctx *c) { (void)c; }
__attribute__((weak)) void __real_f_4000731fdc(Ctx *c) { (void)c; }
__attribute__((weak)) void __real_f_4000a58394(Ctx *c) { (void)c; }
