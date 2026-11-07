/* Track O (options), first set (track V session 4): config\graphics.ini.
 *
 * Path: SMO_GRAPHICS_CONFIG=<file> when set; else graphics.ini beside the file SMO_CONFIG names (SMO_CONFIG is the
 * path of controllers.ini, nnpad_sdl.c); else <exe folder>\..\config\graphics.ini. Written with the defaults and their
 * documentation when it does not exist. Read ONCE at start (the window, the device and the swapchain are made from it).
 * Environment switches of the test scripts (SMO_RENDERER, SMO_VK_DEVICE, SMO_VK_PRESENT) win over the file.
 * The controllers.ini reader of nnpad_sdl.c is built around its device sections and is not reusable here: this file has
 * its own 30-line reader (key = value, ';' or '#' comments, section headers ignored). */
#include "plat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gfx_options.h"

/* 0.2, Android: the settings the 60 fps mode was measured with on a Galaxy S24 Ultra (Cascade, one run each: 58.1 fps at 310 MHz against 44.6 at
 * 231 MHz with the old defaults, which stay on the other platforms). None of them needs a particular chipset: mailbox falls back when a GPU
 * does not offer it. An existing graphics.ini keeps its values. */
#ifdef __ANDROID__
#define DEF_VSYNC "mailbox"
#define DEF_SCALE "1.00"
#define DEF_BOOST "on"
#define DEF_AA "fxaa"
#define DEF_SHARPEN "75"
#else
#define DEF_VSYNC "clock"
#define DEF_SCALE "auto"
#define DEF_BOOST "auto"
#define DEF_AA "off"
#define DEF_SHARPEN "0"
#endif
static const char *const k_default_ini =
"; graphics.ini: display and rendering options of the native build (read once at start by gfx_options.c).\r\n"
"; Delete the file to get it written again with these defaults. A line starting with ';' is a comment.\r\n"
"; Every value below is the default.\r\n"
"\r\n"
"; renderer = vulkan | opengl\r\n"
";   vulkan: the default since 2026-10-09. opengl: the first renderer (NVIDIA only), kept as a fallback.\r\n"
"renderer = vulkan\r\n"
"\r\n"
"; window = borderless_window | borderless_fullscreen | windowed | fullscreen\r\n"
";   borderless_window:     a window without a frame or title bar, the size below, centred on the screen. Drag it\r\n"
";                          anywhere to move it; double-click it for borderless_fullscreen and back.\r\n"
";   borderless_fullscreen: no frame, covers the monitor the window is on (the desktop keeps its resolution).\r\n"
";   windowed:              a normal window with a title bar, the client size below.\r\n"
";   fullscreen:            as borderless_fullscreen, and always on top. (Exclusive fullscreen with a mode change is\r\n"
";                          not implemented.)\r\n"
";   F11 switches between covering the monitor and the window while the game runs.\r\n"
"window = borderless_window\r\n"
"width = 1280\r\n"
"height = 720\r\n"
"\r\n"
"; vsync = clock | on | off | mailbox      (Vulkan)\r\n"
";   clock:   frames are shown at once (mailbox when the GPU offers it, else immediate) and the game is paced by the\r\n"
";            program's own 60 Hz clock. Lowest latency; can tear only when mailbox is missing.\r\n"
";   on:      the display's own vertical sync (FIFO). No tearing; on a display that is not 60 Hz the two clocks drift.\r\n"
";   off:     immediate presentation, tearing allowed. The game still runs at 60 frames per second.\r\n"
";   mailbox: mailbox or nothing (falls back to clock's choice with a message when the GPU does not offer it).\r\n"
"vsync = " DEF_VSYNC "\r\n"
"\r\n"
"; gpu = auto | <number> | <part of the name>      (Vulkan)\r\n"
";   auto: a discrete GPU first, then an integrated one. The log lists the devices with their numbers:\r\n"
";   'nvn_vk: device 0: ...'. Examples: gpu = 1      gpu = Intel      gpu = RTX\r\n"
"gpu = auto\r\n"
"\r\n"
"; console_mode = docked | handheld\r\n"
";   What nn::oe::GetOperationMode answers. MEASURED on 13.0.4 (2026-10-09): with handheld the game still makes its\r\n"
";   1920x1080 window texture, so this alone does not lower the render size; kept for study, leave it at docked.\r\n"
"console_mode = docked\r\n"
"\r\n"
"; resolution_scale = 1.0\r\n"
";   Internal resolution scale, 0.25 to 1.0 (Vulkan only; OpenGL ignores it). 1.0 = the game's 1920x1080. 0.75 = 1440x810,\r\n"
";   0.5 = 960x540: the main switch for a weak GPU. The whole picture is drawn smaller, the 2D interface too (it gets\r\n"
";   softer), and is scaled to the window size. Details and limits: plan\\phases\\O1_options.md.\r\n"
"; Android port: auto draws as the console shows the game: docked 1920x1080 (1.2 x the game's 1600x900),\r\n"
";   handheld 1280x720 (the game's own size). A number (0.25 .. 2.0) is a scale of the game's own sizes instead.\r\n"
"resolution_scale = " DEF_SCALE "\r\n"
"\r\n"
"; frame_rate = 60 | 30\r\n"
";   60: the console's frame rate. 30: 30 frames a second are drawn and the game's logic runs twice for each of them,\r\n"
";   so the game keeps its full speed (for a device that cannot draw 60). Animation is less smooth.\r\n"
"frame_rate = 60\r\n"
"\r\n"
"; fps_cap = 60      at most this many frames a second (20 .. 60; 60 = no limit). The game advances one step per\r\n"
";   frame, so below 60 it runs in slow motion; frame_rate = 30 keeps full speed and is used instead when set.\r\n"
"fps_cap = 60\r\n"
"\r\n"
"; boost = auto | on | off      (Android) auto: while frames come later than the frame rate asks, the threads that set it\r\n"
";   keep their CPUs busy instead of sleeping, so the phone raises the clocks (more fps in heavy scenes, more heat\r\n"
";   while it is on); it stops by itself when the frames are on time again. on: always, also where it gains nothing.\r\n"
"boost = " DEF_BOOST "\r\n"
"\r\n"
"; anti_aliasing = off | fxaa      (Vulkan) fxaa: edges smoothed on the final picture; a little softer, cheap\r\n"
"anti_aliasing = " DEF_AA "\r\n"
"\r\n"
"; sharpening = 0 .. 100      (Vulkan) contrast-adaptive: detail sharper, hard edges left alone (0 = off)\r\n"
"sharpening = " DEF_SHARPEN "\r\n"

"\r\n"
"; fps_counter = on | off      (Android: the frame rate in the corner)\r\n"
"fps_counter = on\r\n"
"\r\n"
"; driver = custom | system    (Android: custom = a Vulkan driver package in files/driver when there is one)\r\n"
"driver = custom\r\n"
"\r\n"
"; anisotropic_filtering = game | 2 | 4 | 8 | 16      (Vulkan)\r\n"
";   game: each texture sampler keeps the value the game gave it. A number: at least that much on every sampler that\r\n"
";   filters between mip levels (sharper textures at flat angles; changes the picture, costs little on any GPU).\r\n"
"anisotropic_filtering = game\r\n"
"\r\n"
"; shader_wait = hold | skip      (Vulkan)\r\n"
";   What happens when a draw needs a shader or pipeline that is not ready yet (mostly the first time something is shown\r\n"
";   on this PC). hold: the frame waits for it (a late frame, a correct picture). skip: in and around a fight the draw is\r\n"
";   left out until it is ready (no late frame; the object or effect is missing for some frames). The log counts both.\r\n"
"shader_wait = hold\r\n"
"\r\n"
"; diagnostics = off | on      (Vulkan, test option)\r\n"
";   on: on Mali GPUs, about once a minute a frame is measured and small pictures of it are kept for the log export (a short hitch).\r\n"
"diagnostics = off\r\n"
"\r\n"
"; profile = custom | low\r\n"
";   custom: every key of this file as written. low: the set measured for a weak or integrated GPU (2026-10-12, Intel\r\n"
";   integrated: 4.2 ms of GPU time per fight frame instead of 9.0): resolution_scale = 0.5, anisotropic_filtering = game,\r\n"
";   vsync = clock; those three keys of the file are then not used. The log says which profile is active.\r\n"
"profile = custom\r\n"
"\r\n"
"; frame_time_overlay = off | on\r\n"
";   on: the average frame time of the last second in the window title ('.. fps - 16.67 ms per frame') and as a\r\n"
";   'frame-time:' line in the log. (Bars drawn into the picture are not implemented yet.)\r\n"
"frame_time_overlay = off\r\n";

#define GFX_LOW_SCALE 0.5f
#define GFX_LOW_SCALE_TEXT "0.50"
static GfxOptions g_opt;
static plat_once g_once = PLAT_ONCE_INIT;

static void trim(char *s) {
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = 0;
    char *b = s;
    while (*b == ' ' || *b == '\t') b++;
    if (b != s) memmove(s, b, strlen(b) + 1);
}
static void opt_path(char *out) {
    const char *e = getenv("SMO_GRAPHICS_CONFIG"), *c = getenv("SMO_CONFIG");
    char tmp[PLAT_MAX_PATH], exe[PLAT_MAX_PATH];
    if (e && e[0]) { snprintf(out, PLAT_MAX_PATH, "%s", e); return; }
    if (c && c[0]) {
        snprintf(tmp, sizeof tmp, "%s", c);
        char *s = strrchr(tmp, '\\'), *s2 = strrchr(tmp, '/');
        if (s2 > s) s = s2;
        if (s) { *s = 0; snprintf(out, PLAT_MAX_PATH, "%s/graphics.ini", tmp); } else snprintf(out, PLAT_MAX_PATH, "graphics.ini");
        return;
    }
#ifdef _WIN32
    GetModuleFileNameA(NULL, exe, PLAT_MAX_PATH);
    char *s = strrchr(exe, '\\');
    if (s) *s = 0;
    snprintf(tmp, sizeof tmp, "%s\\..\\config\\graphics.ini", exe);
    if (!_fullpath(out, tmp, PLAT_MAX_PATH)) snprintf(out, PLAT_MAX_PATH, "%s", tmp);
#else
    (void)exe;
    snprintf(out, PLAT_MAX_PATH, "%s/config/graphics.ini", plat_data_root());   /* Android: the app's data folder */
#endif
}
static void opt_load(void) {
    GfxOptions *o = &g_opt;
    int written = 0, unknown = 0;
    memset(o, 0, sizeof *o);
    o->width = 1280; o->height = 720; o->scale = 0.0f; o->window_mode = 3; o->fps_counter = 1; o->dynres = 1; o->boost = 1; o->diag_off = 1;
#ifdef __ANDROID__
    o->present = 3; o->scale = 1.0f; o->boost = 2; o->fxaa = 1; o->sharpen = 0.75f;      /* as DEF_* above: a file without these keys */
#endif
    opt_path(o->path);
    FILE *f = fopen(o->path, "rb");
    if (!f) {
        char b[PLAT_MAX_PATH];
        snprintf(b, sizeof b, "%s", o->path);
        { char *cut = strrchr(b, '/'), *cut2 = strrchr(b, '\\'); if (cut2 > cut) cut = cut2; if (cut) { *cut = 0; plat_mkdirs(b); } }
        if ((f = fopen(o->path, "wb")) != NULL) { fwrite(k_default_ini, 1, strlen(k_default_ini), f); fclose(f); written = 1; }
        f = fopen(o->path, "rb");
    }
    char line[512];
    while (f && fgets(line, sizeof line, f)) {
        char *eq, *key = line, *val;
        trim(line);
        if (!line[0] || line[0] == ';' || line[0] == '#' || line[0] == '[' || !(eq = strchr(line, '='))) continue;
        *eq = 0; val = eq + 1;
        trim(key); trim(val);
        if (!plat_stricmp(key, "renderer")) o->renderer = !plat_stricmp(val, "opengl") || !plat_stricmp(val, "gl");
        else if (!plat_stricmp(key, "window")) o->window_mode = !plat_stricmp(val, "borderless_fullscreen") || !plat_stricmp(val, "borderless") ? 1 : !plat_stricmp(val, "fullscreen") ? 2 : !plat_stricmp(val, "windowed") ? 0 : 3;
        else if (!plat_stricmp(key, "width")) { int v = atoi(val); if (v >= 320 && v <= 16384) o->width = v; }
        else if (!plat_stricmp(key, "height")) { int v = atoi(val); if (v >= 180 && v <= 16384) o->height = v; }
        else if (!plat_stricmp(key, "vsync")) o->present = !plat_stricmp(val, "on") ? 1 : !plat_stricmp(val, "off") ? 2 : !plat_stricmp(val, "mailbox") ? 3 : 0;
        else if (!plat_stricmp(key, "gpu")) { if (plat_stricmp(val, "auto")) snprintf(o->gpu, sizeof o->gpu, "%s", val); }
        else if (!plat_stricmp(key, "console_mode")) o->handheld = !plat_stricmp(val, "handheld");
        else if (!plat_stricmp(key, "resolution_scale")) { float v = (float)atof(val); o->scale = !plat_stricmp(val, "auto") ? 0.0f : v > 0 ? v : o->scale; }
        else if (!plat_stricmp(key, "anisotropic_filtering")) { int v = atoi(val); o->aniso = v == 2 || v == 4 || v == 8 || v == 16 ? v : 0; }
        else if (!plat_stricmp(key, "frame_time_overlay")) o->overlay = !plat_stricmp(val, "on") || !strcmp(val, "1");
        else if (!plat_stricmp(key, "profile")) o->profile = !plat_stricmp(val, "low");
        else if (!plat_stricmp(key, "frame_rate")) o->fps30 = atoi(val) == 30;
        else if (!plat_stricmp(key, "fps_cap")) { int v = (int)(atof(val) + 0.5); o->fps_cap = v >= 10 && v < 60 ? v : 0; }
        else if (!plat_stricmp(key, "sharpening")) { float v = (float)atof(val); o->sharpen = v < 0 ? 0 : v > 100 ? 1.0f : v / 100.0f; }
        else if (!plat_stricmp(key, "dynamic_resolution")) o->dynres = plat_stricmp(val, "off") != 0;
        else if (!plat_stricmp(key, "anti_aliasing")) o->fxaa = !plat_stricmp(val, "fxaa");
        else if (!plat_stricmp(key, "boost")) o->boost = !plat_stricmp(val, "off") ? 0 : !plat_stricmp(val, "on") ? 2 : 1;
        else if (!plat_stricmp(key, "fps_counter")) o->fps_counter = plat_stricmp(val, "off") != 0;
        else if (!plat_stricmp(key, "driver")) o->driver_system = !plat_stricmp(val, "system");
        else if (!plat_stricmp(key, "shader_wait")) o->shader_skip = !plat_stricmp(val, "skip");
        else if (!plat_stricmp(key, "diagnostics")) o->diag_off = plat_stricmp(val, "on") != 0;
        else unknown++;
    }
    if (f) fclose(f);
    if (o->profile == 1) {       /* the low-end profile wins over the three keys it sets, wherever they stand in the file (values: O1_options.md, session 9) */
        o->scale = GFX_LOW_SCALE; o->aniso = 0; o->present = 0;
    }
    printf("graphics profile: %s\n", o->profile == 1 ? "low (weak or integrated GPU): resolution_scale=" GFX_LOW_SCALE_TEXT ", anisotropic_filtering=game, vsync=clock; these three keys of the file are not used"
                                                     : "custom (every key of graphics.ini as written; 'profile = low' is the set for a weak or integrated GPU)");
    static const char *const k_win[4] = {"windowed", "borderless_fullscreen", "fullscreen", "borderless_window"}, *const k_vs[4] = {"clock", "on", "off", "mailbox"};
    printf("graphics options: %s (%s): renderer=%s window=%s %dx%d vsync=%s gpu=%s console_mode=%s resolution_scale=%.2f anisotropic_filtering=%d frame_time_overlay=%s%s\n", o->path,
           written ? "written with the defaults" : f ? "read" : "NOT readable, defaults used", o->renderer ? "opengl" : "vulkan", k_win[o->window_mode], o->width, o->height, k_vs[o->present],
           o->gpu[0] ? o->gpu : "auto", o->handheld ? "handheld" : "docked", o->scale, o->aniso, o->overlay ? "on" : "off", unknown ? " (lines with an unknown key were ignored)" : "");
    if (o->scale != 1.0f && o->renderer) printf("graphics options: resolution_scale=%.2f is ignored by the OpenGL renderer (Vulkan only)\n", o->scale);
    if (o->scale != 1.0f && (o->scale < 0.25f || o->scale > 1.0f)) printf("graphics options: resolution_scale=%.2f is outside 0.25 .. 1.0: the nearest of the two is used\n", o->scale);
    fflush(stdout);
}
const GfxOptions *gfx_options(void) {
    plat_once_run(&g_once, opt_load);
    return &g_opt;
}
