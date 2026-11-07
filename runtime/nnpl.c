/* nn::pl: the system's shared fonts, read from plain TrueType files on the host.
 *
 * The files come from the user's own firmware dump (extract-tools/fw_fonts.py) and are looked up in
 * %SMO_FONTS% or ..\sysdata\fonts. A font that is missing reports size 0 but still "loaded", so the
 * game does not wait for it forever.
 *
 * SharedFontType: 0 Standard, 1 ChineseSimple, 2 ChineseSimpleExtension, 3 ChineseTraditional,
 *                 4 Korean, 5 NintendoExtension. SharedFontLoadState: 0 Loading, 1 Loaded. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "plat.h"
#include "runtime.h"
#include "hostrt.h"

#define A0 (c->x[0])
#define RET(v) (c->x[0] = (uint64_t)(v))
#define FONT_TYPES 7

static const char *const font_files[FONT_TYPES] = {
    "nintendo_udsg-r_std_003.ttf", "nintendo_udsg-r_org_zh-cn_003.ttf", "nintendo_udsg-r_ext_zh-cn_003.ttf",
    "nintendo_udjxh-db_zh-tw_003.ttf", "nintendo_udsg-r_ko_003.ttf", "nintendo_ext_003.ttf", "nintendo_ext2_003.ttf",
};
static struct { void *data; size_t size; int tried; } g_fonts[FONT_TYPES];
static plat_mutex g_lock;

static void load_font(unsigned type) {
    if (type >= FONT_TYPES) return;
    plat_mutex_lock(&g_lock);
    if (!g_fonts[type].tried) {
        g_fonts[type].tried = 1;
        char path[PLAT_MAX_PATH];
        const char *dir = getenv("SMO_FONTS");
        snprintf(path, sizeof path, "%s" PLAT_SEP_S "%s", dir ? dir : ".." PLAT_SEP_S "sysdata" PLAT_SEP_S "fonts", font_files[type]);
        FILE *f = fopen(path, "rb");
#ifdef __ANDROID__
        /* Android port: without the firmware's font (tools/extract/fw_fonts.py, pushed by tools/android/push_data.py)
         * the device's own font stands in, so the game still starts. The game's text uses its own fonts (RomFS
         * LayoutData/FontData.szs); the shared font is a fallback for system text, and the Nintendo extension symbols
         * have no substitute: such glyphs are missing. The log says SUBSTITUTE. */
        static const char *const sub_latin[] = {"/system/fonts/RobotoStatic-Regular.ttf", "/system/fonts/Roboto-Regular.ttf", "/system/fonts/DroidSans.ttf"};
        static const char *const sub_cjk[] = {"/system/fonts/NotoSansCJK-Regular.ttc", "/system/fonts/DroidSansFallback.ttf"};
        int cjk = type >= 1 && type <= 4;
        for (int i = 0; !f && i < (cjk ? 2 : 3); i++) {
            f = fopen(cjk ? sub_cjk[i] : sub_latin[i], "rb");
            if (f) { printf("nn::pl: shared font %u: SUBSTITUTE %s for the missing %s (not the console's font)\n", type, cjk ? sub_cjk[i] : sub_latin[i], path); snprintf(path, sizeof path, "%s", cjk ? sub_cjk[i] : sub_latin[i]); }
        }
#endif
        if (f) {
            fseek(f, 0, SEEK_END);
            long size = ftell(f);
            fseek(f, 0, SEEK_SET);
            void *data = plat_aligned_malloc((size_t)size + 16, 4096);
            if (data && fread(data, 1, (size_t)size, f) == (size_t)size) { g_fonts[type].data = data; g_fonts[type].size = (size_t)size; }
            fclose(f);
        }
        printf("nn::pl: shared font %u %s (%s, %zu bytes)\n", type, g_fonts[type].data ? "loaded" : "NOT FOUND", path, g_fonts[type].size);
        if (!g_fonts[type].data) {                        /* close4: a console always has its system fonts; "loaded, size 0" was a made-up state */
            printf("nn::pl: the system font file is a required input (extract-tools/fw_fonts.py from your own firmware dump, or SMO_FONTS=<folder>): stopping\n");
            fflush(stdout);
            report_and_exit(7);
        }
    }
    plat_mutex_unlock(&g_lock);
}

static void pl_RequestSharedFontLoad(Ctx *c) { load_font((unsigned)A0); }
static void pl_GetSharedFontLoadState(Ctx *c) { load_font((unsigned)A0); RET(1); }
static void pl_GetSharedFontAddress(Ctx *c) { unsigned t = (unsigned)A0; load_font(t); RET(t < FONT_TYPES ? (uint64_t)(uintptr_t)g_fonts[t].data : 0); }
static void pl_GetSharedFontSize(Ctx *c) { unsigned t = (unsigned)A0; load_font(t); RET(t < FONT_TYPES ? g_fonts[t].size : 0); }
static void pl_IsVrMode(Ctx *c) { RET(0); }
static void nothing(Ctx *c) { (void)c; }

static const ImportDef pl_imports[] = {
    {"_ZN2nn2pl21RequestSharedFontLoadENS0_14SharedFontTypeE", pl_RequestSharedFontLoad},
    {"_ZN2nn2pl22GetSharedFontLoadStateENS0_14SharedFontTypeE", pl_GetSharedFontLoadState},
    {"_ZN2nn2pl20GetSharedFontAddressENS0_14SharedFontTypeE", pl_GetSharedFontAddress},
    {"_ZN2nn2pl17GetSharedFontSizeENS0_14SharedFontTypeE", pl_GetSharedFontSize},
    {"_ZN2nn2pl8IsVrModeEv", pl_IsVrMode}, {"_ZN2nn2pl11BeginVrModeEv", nothing}, {"_ZN2nn2pl9EndVrModeEv", nothing},
};

void register_pl_imports(void) {
    plat_mutex_init(&g_lock);
    register_imports(pl_imports, (int)(sizeof pl_imports / sizeof pl_imports[0]));
}
