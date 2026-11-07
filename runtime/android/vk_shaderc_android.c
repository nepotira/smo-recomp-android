/* vk_shaderc_android.c - GLSL (Vulkan dialect) -> SPIR-V on Android: the same interface as vk_shaderc_shim.c, with shaderc
 * (the NDK's own sources, built by tools/android/build_shaderc.py) linked in instead of a DLL loaded at run time.
 * Settings as the shim's: Vulkan 1.3 environment, SPIR-V 1.6, no optimisation, entry point "main".
 * stage: 0 vertex, 1 fragment, 2 geometry, 5 compute (the MX_* numbers of mx_translate.h). Thread-safe: one compiler
 * object, made once; shaderc compilers may be used from several threads. Android port (2026-10-27). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <shaderc/shaderc.h>
#include "plat.h"

static shaderc_compiler_t g_compiler;
static shaderc_compile_options_t g_options, g_options_opt;
extern int mx_opt_fs, mx_precise;
/* 0.1.7 (mx_precise): every float operation of a vertex or geometry stage gets NoContraction, so no compiler fuses or splits it. The depth
 * pass and the colour pass are different shaders computing one position, and a test of EQUAL or LEQUAL against it leaves holes when they
 * differ. GLSL's precise only reaches what glslang traces back from a result, which translated register code defeats. Approach and
 * instruction list from Power Moon Recomp (MIT, docs/licenses/). */
static int float_arith(const uint32_t *w, uint32_t wc, uint32_t op) {
    switch (op) {
    case 127: case 129: case 131: case 133: case 136:      /* FNegate FAdd FSub FMul FDiv */
    case 142: case 143: case 144: case 145: case 146: case 148:      /* vector and matrix products, Dot */
        return 1;
    case 12: return wc >= 5 && w[4] == 50;                 /* ExtInst GLSL.std.450 Fma */
    default: return 0;
    }
}
static uint32_t *no_contraction(uint32_t *spv, size_t *nwords) {
    enum { OP_DECORATE = 71, DEC_NO_CONTRACTION = 42 };
    size_t n = *nwords, insert = 0, count = 0, o;
    uint32_t bound = spv[3];
    uint8_t *done = calloc(bound, 1);
    if (!done) return spv;
    for (size_t i = 5; i < n;) {
        uint32_t wc = spv[i] >> 16, op = spv[i] & 0xFFFF;
        if (!wc) break;
        if (!insert && op >= 19 && op <= 39) insert = i;   /* the first type: the end of the annotations */
        if (op == OP_DECORATE && wc >= 3 && spv[i + 2] == DEC_NO_CONTRACTION && spv[i + 1] < bound) done[spv[i + 1]] = 1;
        if (float_arith(spv + i, wc, op) && spv[i + 2] < bound) count++;
        i += wc;
    }
    uint32_t *out = insert && count ? malloc((n + count * 3) * 4) : NULL;
    if (!out) { free(done); return spv; }
    memcpy(out, spv, insert * 4);
    o = insert;
    for (size_t i = 5; i < n;) {
        uint32_t wc = spv[i] >> 16, op = spv[i] & 0xFFFF;
        if (!wc) break;
        if (float_arith(spv + i, wc, op) && spv[i + 2] < bound && !done[spv[i + 2]]) {
            done[spv[i + 2]] = 1;
            out[o++] = 3u << 16 | OP_DECORATE; out[o++] = spv[i + 2]; out[o++] = DEC_NO_CONTRACTION;
        }
        i += wc;
    }
    memcpy(out + o, spv + insert, (n - insert) * 4);
    *nwords = o + (n - insert);
    free(done); free(spv);
    return out;
}
static plat_once g_once = PLAT_ONCE_INIT;

static void init_once(void) {
    g_compiler = shaderc_compiler_initialize();
    g_options = shaderc_compile_options_initialize();
    if (!g_options) return;
    shaderc_compile_options_set_target_env(g_options, shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_3);
    shaderc_compile_options_set_target_spirv(g_options, shaderc_spirv_version_1_6);
    g_options_opt = shaderc_compile_options_clone(g_options);      /* 0.1.7: mx_opt_fs */
    if (g_options_opt) shaderc_compile_options_set_optimization_level(g_options_opt, shaderc_optimization_level_performance);
}

void smo_spirv_free(uint32_t *words) { free(words); }

int smo_glsl_to_spirv(int stage, const char *src, uint32_t **words, size_t *count, char *log, size_t log_size) {
    int ok = 0;
    if (log && log_size) log[0] = 0;
    *words = NULL; *count = 0;
    plat_once_run(&g_once, init_once);
    if (!g_compiler || !g_options) { if (log) snprintf(log, log_size, "shaderc initialisation failed"); return 0; }
    shaderc_shader_kind kind = stage == 0 ? shaderc_vertex_shader : stage == 1 ? shaderc_fragment_shader : stage == 2 ? shaderc_geometry_shader
                             : stage == 5 ? shaderc_compute_shader : (shaderc_shader_kind)-1;
    if ((int)kind < 0) { if (log) snprintf(log, log_size, "stage %d is not vertex (0), fragment (1), geometry (2) or compute (5)", stage); return 0; }
    shaderc_compile_options_t use = mx_opt_fs && g_options_opt && (stage == 1 || stage == 5) ? g_options_opt : g_options;
    shaderc_compilation_result_t r = shaderc_compile_into_spv(g_compiler, src, strlen(src), kind, "nvn_stage", "main", use);
    if (use != g_options && r && shaderc_result_get_compilation_status(r) != shaderc_compilation_status_success) {      /* never lose a shader to the optimiser */
        static int said;
        if (!said++) printf("nvn_vk: spirv-opt refused a stage: compiled without it (%s)\n", shaderc_result_get_error_message(r));
        shaderc_result_release(r);
        r = shaderc_compile_into_spv(g_compiler, src, strlen(src), kind, "nvn_stage", "main", g_options);
    }
    if (!r) { if (log) snprintf(log, log_size, "shaderc_compile_into_spv returned no result"); return 0; }
    const char *msg = shaderc_result_get_error_message(r);
    if (msg && msg[0] && log) snprintf(log, log_size, "%s", msg);
    if (shaderc_result_get_compilation_status(r) == shaderc_compilation_status_success) {
        size_t n = shaderc_result_get_length(r) / 4;
        if (n) {
            *words = malloc(n * sizeof(uint32_t));
            if (*words) { memcpy(*words, shaderc_result_get_bytes(r), n * 4); *count = n; ok = 1; }
            if (ok && mx_precise && (stage == 0 || stage == 2)) *words = no_contraction(*words, count);
        }
    }
    shaderc_result_release(r);
    return ok;
}
