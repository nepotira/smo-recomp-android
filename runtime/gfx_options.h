/* Track O (options), first set: config\graphics.ini. See gfx_options.c and plan\phases\O1_options.md. */
#ifndef GFX_OPTIONS_H
#define GFX_OPTIONS_H
typedef struct {
    char path[260];
    int renderer;            /* 0 Vulkan (default), 1 OpenGL                               env SMO_RENDERER=vk|gl wins */
    int window_mode;         /* 0 windowed, 1 borderless fullscreen (desktop size), 2 fullscreen (borderless, topmost, covering the
                              * monitor), 3 borderless window (the default: no frame, width x height, centred) */
    int width, height;       /* client size of the windowed modes (windowed, borderless window) */
    int present;             /* 0 clock (mailbox, else immediate, paced by our 60 Hz clock), 1 vsync on (fifo), 2 vsync off (immediate), 3 mailbox;   env SMO_VK_PRESENT wins */
    char gpu[64];            /* "" = discrete first; an index of the printed list, or a part of the device name   env SMO_VK_DEVICE wins */
    float scale;             /* internal resolution scale asked for (Vulkan: nvn_vk_draw.c rs_scale, 0.25 .. 1.0; env SMO_VK_SCALE wins) */
    int handheld;            /* 1: the game is told it runs handheld = it makes its own 1280x720 render targets instead of 1920x1080 */
    int aniso;               /* 0 = the game's own value; 2, 4, 8, 16 = at least that on samplers that filter between mip levels */
    int overlay;             /* 1: frame time in the title bar and a "frame-time:" log line every second */
    int shader_skip;         /* shader_wait: 0 hold (a draw waits for its shader or pipeline: the frame is late), 1 skip (near a fight the draw is left out until they are ready; nvn_vk_m4.c skip_on; env SMO_VK_NOHOLD wins) */
    int profile;            /* 0 custom (the keys as written), 1 low: scale, aniso and present are set by the profile (gfx_options.c) */
    int fps_counter;         /* fps_counter = on (default) | off: the frame rate in the corner (Android present pass; env SMO_FPS=0 wins) */
    int driver_system;       /* driver = custom (default: a custom Vulkan driver when one is installed, Android) | system (env SMO_DRIVER=system wins) */
    int fxaa;                /* anti_aliasing = fxaa: edges smoothed in the present pass (nvn_vk_draw.c) */
    int dynres;              /* dynamic_resolution = on: the game caps its 3D size by its quality setting; off: full size (smo_quality.c) */
    float sharpen;           /* sharpening = 0..100 (%): contrast-adaptive sharpening in the present pass (0 = off) */
    int fps_cap;             /* fps_cap = 20..59: at most that many frames a second (0 or 60 = none; frame_rate = 30 wins); the game runs slower below 60 */
    int fps30;               /* frame_rate = 30: 30 frames a second are drawn and the game's logic runs twice a frame (full speed, smo_framerate.c) */
    int diag_off;            /* diagnostics = off (default, 1; an old "auto" line counts as off) | on (ARM GPUs: nvn_vk_draw.c diag_present, eq_begin) */
    int boost;               /* boost = auto (default, 1) | on (2, always) | off (0): the frame's threads keep their CPUs' clocks up while frames are late (Android, android_perf.c) */
} GfxOptions;
const GfxOptions *gfx_options(void);      /* any thread; read once (the file is written with defaults when it does not exist) */
#endif
