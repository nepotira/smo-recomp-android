/* nnpad_android.c - nnpad.h on Android: game controllers, the touch screen and the phone's motion sensors.
 *
 * Players: player 1 is the touch controls together with the first controller that sends input (both act at once, so a
 * controller can be picked up at any moment); every further controller is the next player. The phone's sensors are
 * player 1's motion (the console held in the hand).
 *
 * Touch controls (normalised window coordinates; sizes in units of the window's height, so they stay round):
 *   - left of 45% of the width: a floating stick where the finger lands (left stick);
 *   - right of it, away from the buttons: the camera. camera_touch = stick (the default): a floating right stick where
 *     the finger lands, held off its centre the camera keeps turning, faster the further out; camera_touch = drag: the
 *     camera follows the finger's movement (the right stick follows the finger's speed);
 *   - A B X Y as on the console (A right), L ZL top left, R ZR top right, - and + at the top, the d-pad bottom left,
 *     the right stick's click; SHAKE does what shaking the controller does (Cappy's homing throw, spin moves): a short
 *     swing of the motion sensors while held.
 *   The controls hide themselves while a controller is in use and come back at the next touch (touch = auto).
 *   A small ring at the top centre switches them off and on (touch = off | auto, saved in controls.ini): off, touches
 *   reach the game no more and only the ring stays, faint.
 *
 * config/controls.ini (written with these defaults at the first start; read again when it changes):
 *   layout = positional | labels    face buttons: positional = the button where the console's A is (right) is A
 *   touch = auto | on | off         touch_opacity = 0..1   touch_scale = 0.6..1.6
 *   deadzone = 0..0.5 (controller sticks)   camera_speed = 0.2..4 (touch camera)   motion = on | off (phone sensors)
 *   camera_touch = stick | drag
 *   pos_<control> = x y [size]      the player's layout (the launcher's editor writes it): where a control sits, in
 *                                   parts of the window's width and height, and its size (1 = as made). Controls: A B
 *                                   X Y L ZL R ZR MINUS PLUS R3 SHAKE LEFT RIGHT UP DOWN, LSTICK and RSTICK (where the
 *                                   two sticks rest). A control without a line keeps its place below.
 *   bind_<button> = <keycode>       the controller key for a console button (Android KEYCODE_* numbers; the launcher
 *                                   writes them): it replaces the button's own key. Buttons as above, without the
 *                                   sticks and SHAKE.
 * Android port (2026-10-27; layout and bindings 2026-11-03). */
#include <android/input.h>
#include <android/keycodes.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "plat.h"
#include "nnpad.h"
#include "android_host.h"

enum { B_A, B_B, B_X, B_Y, B_LS, B_RS, B_L, B_R, B_ZL, B_ZR, B_PLUS, B_MINUS, B_LEFT, B_UP, B_RIGHT, B_DOWN, B_SHAKE = 32,
       B_EXIT = 33 };      /* not console buttons: SHAKE shakes the phone, EXIT asks to leave the game (android_main.c) */
#define STICK_MAX 32767

typedef struct {
    int id, used, player;
    uint64_t buttons;              /* from keys */
    uint64_t latched;              /* pressed since the last poll (a tap shorter than a frame is not lost) */
    uint64_t hat;                  /* d-pad from HAT_X / HAT_Y */
    float ax[48];                  /* the latest axis values */
    int64_t last_ms;
} Ctl;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static Ctl g_ctl[8];
static int g_nplayers = 1;
static struct { int layout_labels, touch_mode, motion, camera_stick; float opacity, scale, deadzone, camera; } g_cfg = {0, 0, 1, 1, 0.45f, 1.0f, 0.15f, 1.0f};
static float g_accel[3] = {0, -9.80665f, 0}, g_gyro[3];
static int g_has_sensors;
static int64_t g_last_touch_ms, g_last_pad_ms, g_shake_until;

/* ---------------------------------------------------------------- the touch controls */
typedef struct { float x, y, r; int bit; char label[4]; const char *key; } TButton;      /* key: its name in controls.ini */
static const TButton k_buttons[] = {
    {0.930f, 0.700f, 0.070f, B_A, "A", "A"}, {0.855f, 0.835f, 0.070f, B_B, "B", "B"}, {0.855f, 0.565f, 0.070f, B_X, "X", "X"}, {0.780f, 0.700f, 0.070f, B_Y, "Y", "Y"},
    {0.090f, 0.200f, 0.060f, B_L, "L", "L"}, {0.090f, 0.070f, 0.055f, B_ZL, "ZL", "ZL"}, {0.910f, 0.200f, 0.060f, B_R, "R", "R"}, {0.910f, 0.070f, 0.055f, B_ZR, "ZR", "ZR"},
    {0.420f, 0.060f, 0.045f, B_MINUS, "-", "MINUS"}, {0.580f, 0.060f, 0.045f, B_PLUS, "+", "PLUS"}, {0.690f, 0.880f, 0.050f, B_RS, "R3", "R3"},
    {0.690f, 0.540f, 0.055f, B_SHAKE, "~", "SHAKE"},
    {0.250f, 0.870f, 0.042f, B_LEFT, "<", "LEFT"}, {0.330f, 0.870f, 0.042f, B_RIGHT, ">", "RIGHT"}, {0.290f, 0.790f, 0.042f, B_UP, "^", "UP"}, {0.290f, 0.950f, 0.042f, B_DOWN, "v", "DOWN"},
    {0.250f, 0.060f, 0.040f, B_EXIT, "EX", "EXIT"},
};
#define NBUTTONS ((int)(sizeof k_buttons / sizeof k_buttons[0]))
/* The player's layout (pos_ lines of controls.ini, load_config): the buttons, and where the left stick (0) and the camera
 * stick (1) rest, with their sizes. A touch away from the buttons is the left stick's on its side of the line 0.10 to
 * the camera's side of the middle between the two (0.45 of the width for the layout as made). */
static TButton g_btn[NBUTTONS];
static const float k_stick[2][3] = {{0.14f, 0.70f, 1.0f}, {0.56f, 0.72f, 1.0f}};
static float g_stick[2][3];
#define STICK_R(i) (0.13f * g_cfg.scale * g_stick[i][2])
#define KNOB_R(i) (0.06f * g_cfg.scale * g_stick[i][2])
static int stick_side(float x) {                  /* 1 the left stick, 2 the camera */
    float mid = (g_stick[0][0] + g_stick[1][0]) * 0.5f;
    return g_stick[0][0] <= g_stick[1][0] ? (x < mid + 0.10f ? 1 : 2) : (x > mid - 0.10f ? 1 : 2);
}
typedef struct { int id, kind, button; float x0, y0, x, y, px, py; } Finger;      /* kind 1 stick, 2 camera, 3 button, 4 the on/off ring, 5 ignored (controls off) */
#define TOGGLE_X 0.500f
#define TOGGLE_Y 0.060f
#define TOGGLE_R 0.032f
static void save_touch_mode(void);
static Finger g_fingers[10];
static float g_aspect = 16.0f / 9.0f;                /* width / height of the window */
static float g_cam[2];                               /* camera stick, smoothed */

static float dist_h(float ax, float ay, float bx, float by) {            /* distance in units of the height */
    float dx = (ax - bx) * g_aspect, dy = ay - by;
    return sqrtf(dx * dx + dy * dy);
}
static int touch_visible(int64_t now) {              /* auto: hidden from a controller's input until the next touch */
    (void)now;
    if (g_cfg.touch_mode == 2) return 0;
    if (g_cfg.touch_mode == 1) return 1;
    return g_last_touch_ms >= g_last_pad_ms;
}
static int hit_button(float x, float y) {
    int best = -1;
    float bd = 1e9f;
    for (int i = 0; i < NBUTTONS; i++) {
        float d = dist_h(x, y, g_btn[i].x, g_btn[i].y), r = g_btn[i].r * g_cfg.scale * 1.25f;
        if (d < r && d < bd) { bd = d; best = i; }
    }
    return best;
}
void android_touch(int pointer, float x, float y, int down) {
    int64_t now = (int64_t)plat_ms64();
    int ask_exit = 0;
    pthread_mutex_lock(&g_lock);
    g_last_touch_ms = now;
    Finger *f = NULL;
    for (int i = 0; i < 10; i++) if (g_fingers[i].kind && g_fingers[i].id == pointer) f = &g_fingers[i];
    if (!down) { if (f) f->kind = 0; pthread_mutex_unlock(&g_lock); return; }
    if (!f) {                                          /* a new finger */
        for (int i = 0; i < 10 && !f; i++) if (!g_fingers[i].kind) f = &g_fingers[i];
        if (!f) { pthread_mutex_unlock(&g_lock); return; }
        int b = hit_button(x, y);
        f->id = pointer; f->x0 = f->x = f->px = x; f->y0 = f->y = f->py = y;
        if (dist_h(x, y, TOGGLE_X, TOGGLE_Y) < TOGGLE_R * g_cfg.scale * 1.6f) {      /* the on/off ring */
            f->kind = 4;
            g_cfg.touch_mode = g_cfg.touch_mode == 2 ? 0 : 2;
            save_touch_mode();
        } else if (g_cfg.touch_mode == 2) f->kind = 5;      /* controls off: the touch reaches the game no more */
        else if (b >= 0) { f->kind = 3; f->button = b; ask_exit = g_btn[b].bit == B_EXIT; }
        else f->kind = stick_side(x);
    } else {
        f->x = x; f->y = y;
        if (f->kind == 3 && g_btn[f->button].bit <= B_Y) {     /* sliding over the face buttons presses the one under the finger */
            int b = hit_button(x, y);
            if (b >= 0 && g_btn[b].bit <= B_Y) f->button = b;
        }
    }
    pthread_mutex_unlock(&g_lock);
    if (ask_exit) android_exit_request();
}
/* the touch pad's state; called with g_lock held */
static void touch_state(NnPadState *o, int64_t now) {
    float lx = 0, ly = 0, cx = 0, cy = 0;
    for (int i = 0; i < 10; i++) {
        Finger *f = &g_fingers[i];
        if (f->kind == 3) {
            int bit = g_btn[f->button].bit;
            if (bit == B_SHAKE) g_shake_until = now + 120;
            else if (bit <= B_DOWN) o->buttons |= 1ull << bit;
        } else if (f->kind == 1) {
            float r = STICK_R(0), dx = (f->x - f->x0) * g_aspect / r, dy = (f->y - f->y0) / r, m = sqrtf(dx * dx + dy * dy);
            if (m > 1) { dx /= m; dy /= m; }
            lx = dx; ly = -dy;
        } else if (f->kind == 2 && g_cfg.camera_stick) {      /* the camera stick: its deflection, as the left stick */
            float r = STICK_R(1), dx = (f->x - f->x0) * g_aspect / r, dy = (f->y - f->y0) / r, m = sqrtf(dx * dx + dy * dy);
            if (m > 1) { dx /= m; dy /= m; }
            if (m > 0.08f) { cx = dx * (g_cfg.camera < 1 ? g_cfg.camera : 1); cy = dy * (g_cfg.camera < 1 ? g_cfg.camera : 1); }
        } else if (f->kind == 2) {                     /* the camera: the finger's movement since the last poll */
            cx += (f->x - f->px) * g_aspect * 40.0f * g_cfg.camera;
            cy += (f->y - f->py) * 40.0f * g_cfg.camera;
            f->px = f->x; f->py = f->y;
        }
    }
    if (g_cfg.camera_stick) { g_cam[0] = cx; g_cam[1] = cy; }      /* a stick: as it is held */
    else { g_cam[0] = g_cam[0] * 0.5f + cx * 0.5f; g_cam[1] = g_cam[1] * 0.5f + cy * 0.5f; }
    float c0 = g_cam[0] > 1 ? 1 : g_cam[0] < -1 ? -1 : g_cam[0], c1 = g_cam[1] > 1 ? 1 : g_cam[1] < -1 ? -1 : g_cam[1];
    if (lx || ly) { o->lx = (int)(lx * STICK_MAX); o->ly = (int)(ly * STICK_MAX); }
    if (fabsf(c0) > 0.02f || fabsf(c1) > 0.02f) { o->rx = (int)(c0 * STICK_MAX); o->ry = (int)(-c1 * STICK_MAX); }
}
int android_touch_shapes(TouchShape *out, int max, float *opacity) {
    int n = 0;
    int64_t now = (int64_t)plat_ms64();
    pthread_mutex_lock(&g_lock);
    if (n < max) {                                     /* the on/off ring: always there, faint while the controls are off */
        out[n].x = TOGGLE_X; out[n].y = TOGGLE_Y; out[n].r = TOGGLE_R * g_cfg.scale; out[n].round = 2;
        out[n].pressed = g_cfg.touch_mode != 2; snprintf(out[n].label, sizeof out[n].label, "T"); n++;
    }
    if (touch_visible(now)) {
        uint64_t held = 0;
        for (int i = 0; i < 10; i++) if (g_fingers[i].kind == 3) held |= 1ull << g_fingers[i].button;
        for (int i = 0; i < NBUTTONS && n < max; i++) {
            TouchShape *s = &out[n++];
            s->x = g_btn[i].x; s->y = g_btn[i].y; s->r = g_btn[i].r * g_cfg.scale; s->round = 1; s->pressed = (int)(held >> i & 1);
            snprintf(s->label, sizeof s->label, "%s", g_btn[i].label);
        }
        float bx = g_stick[0][0], by = g_stick[0][1], kx = bx, ky = by;          /* the stick: its base where the finger landed, or at rest */
        for (int i = 0; i < 10; i++) if (g_fingers[i].kind == 1) {
            bx = g_fingers[i].x0; by = g_fingers[i].y0;
            float r = STICK_R(0), dx = (g_fingers[i].x - bx) * g_aspect, dy = g_fingers[i].y - by, m = sqrtf(dx * dx + dy * dy);
            if (m > r) { dx *= r / m; dy *= r / m; }
            kx = bx + dx / g_aspect; ky = by + dy;
        }
        if (n + 2 <= max) {
            out[n].x = bx; out[n].y = by; out[n].r = STICK_R(0); out[n].round = 2; out[n].pressed = 0; out[n].label[0] = 0; n++;
            out[n].x = kx; out[n].y = ky; out[n].r = KNOB_R(0); out[n].round = 1; out[n].pressed = kx != bx || ky != by; snprintf(out[n].label, sizeof out[n].label, "LS"); n++;
        }
        if (g_cfg.camera_stick && n + 2 <= max) {      /* the camera stick: at rest between the d-pad and the face buttons, or where the finger landed */
            float cbx = g_stick[1][0], cby = g_stick[1][1], ckx, cky;
            for (int i = 0; i < 10; i++) if (g_fingers[i].kind == 2) { cbx = g_fingers[i].x0; cby = g_fingers[i].y0; }
            ckx = cbx; cky = cby;
            for (int i = 0; i < 10; i++) if (g_fingers[i].kind == 2) {
                float r = STICK_R(1), dx = (g_fingers[i].x - cbx) * g_aspect, dy = g_fingers[i].y - cby, m = sqrtf(dx * dx + dy * dy);
                if (m > r) { dx *= r / m; dy *= r / m; }
                ckx = cbx + dx / g_aspect; cky = cby + dy;
            }
            out[n].x = cbx; out[n].y = cby; out[n].r = STICK_R(1); out[n].round = 2; out[n].pressed = 0; out[n].label[0] = 0; n++;
            out[n].x = ckx; out[n].y = cky; out[n].r = KNOB_R(1); out[n].round = 1; out[n].pressed = ckx != cbx || cky != cby; snprintf(out[n].label, sizeof out[n].label, "RS"); n++;
        }
    }
    *opacity = g_cfg.opacity;
    pthread_mutex_unlock(&g_lock);
    return n;
}
void android_touch_aspect(float aspect) { if (aspect > 0.2f) g_aspect = aspect; }

/* ---------------------------------------------------------------- controllers */
static Ctl *ctl_for(int device, int64_t now) {      /* g_lock held */
    for (int i = 0; i < 8; i++) if (g_ctl[i].used && g_ctl[i].id == device) { g_ctl[i].last_ms = now; return &g_ctl[i]; }
    for (int i = 0; i < 8; i++) if (!g_ctl[i].used) {
        memset(&g_ctl[i], 0, sizeof g_ctl[i]);
        g_ctl[i].used = 1; g_ctl[i].id = device; g_ctl[i].last_ms = now;
        int p = 0;                                    /* the first controller joins player 1 (with the touch pad), the others the next free players */
        for (int taken = 1; taken; ) { taken = 0; for (int k = 0; k < 8; k++) if (k != i && g_ctl[k].used && g_ctl[k].player == p) { taken = 1; p++; } }
        g_ctl[i].player = p;
        if (p + 1 > g_nplayers) g_nplayers = p + 1;
        printf("nnpad: controller %d is player %d\n", device, p + 1);
        return &g_ctl[i];
    }
    return NULL;
}
static int key_bit_default(int code) {
    switch (code) {
    case AKEYCODE_BUTTON_A: return g_cfg.layout_labels ? B_A : B_B;      /* the bottom button: B positionally */
    case AKEYCODE_BUTTON_B: return g_cfg.layout_labels ? B_B : B_A;      /* the right one: A */
    case AKEYCODE_BUTTON_X: return g_cfg.layout_labels ? B_X : B_Y;      /* the left one: Y */
    case AKEYCODE_BUTTON_Y: return g_cfg.layout_labels ? B_Y : B_X;      /* the top one: X */
    case AKEYCODE_BUTTON_L1: return B_L;
    case AKEYCODE_BUTTON_R1: return B_R;
    case AKEYCODE_BUTTON_L2: return B_ZL;
    case AKEYCODE_BUTTON_R2: return B_ZR;
    case AKEYCODE_BUTTON_THUMBL: return B_LS;
    case AKEYCODE_BUTTON_THUMBR: return B_RS;
    case AKEYCODE_BUTTON_START: return B_PLUS;
    case AKEYCODE_BUTTON_SELECT: case AKEYCODE_BACK: return B_MINUS;
    case AKEYCODE_DPAD_LEFT: return B_LEFT;
    case AKEYCODE_DPAD_UP: return B_UP;
    case AKEYCODE_DPAD_RIGHT: return B_RIGHT;
    case AKEYCODE_DPAD_DOWN: return B_DOWN;
    }
    return -1;
}
/* The player's bindings (bind_ lines, load_config): g_bind[b] the key code for console button b, 0 = its own key. */
#define NKEYS 512
static int g_bind[B_DOWN + 1];
static int g_exit_key = AKEYCODE_BUTTON_MODE;      /* bind_EXIT: the controller key that asks to leave the game (the Home / guide button) */
static int key_bit(int code) {                    /* g_lock held */
    for (int b = 0; b <= B_DOWN; b++) if (g_bind[b] == code) return b;
    int bit = key_bit_default(code);
    return bit >= 0 && g_bind[bit] ? -1 : bit;     /* a button moved to another key no longer answers its own */
}
static uint64_t g_back_key;                           /* the back gesture: Minus of player 1 while it is held */
int android_pad_key(int device, int code, int down) {
    int64_t now = (int64_t)plat_ms64();
    if (device == ANDROID_PAD_BACK_GESTURE) { g_back_key = down ? 1ull << B_MINUS : 0; return 1; }
    pthread_mutex_lock(&g_lock);
    if (code == g_exit_key) { pthread_mutex_unlock(&g_lock); if (down) android_exit_request(); return 1; }
    int bit = key_bit(code);
    if (bit < 0) { pthread_mutex_unlock(&g_lock); return key_bit_default(code) >= 0; }     /* a game key either way: not passed on */
    Ctl *c = ctl_for(device, now);
    if (c) { if (down) { c->buttons |= 1ull << bit; c->latched |= 1ull << bit; } else c->buttons &= ~(1ull << bit); g_last_pad_ms = now; }
    pthread_mutex_unlock(&g_lock);
    return 1;
}
void android_pad_axes(int device, const float *axes, int count) {
    int64_t now = (int64_t)plat_ms64();
    pthread_mutex_lock(&g_lock);
    Ctl *c = ctl_for(device, now);
    if (c) {
        int moved = 0;
        for (int a = 0; a < count && a < 48; a++) { if (fabsf(axes[a] - c->ax[a]) > 0.2f) moved = 1; c->ax[a] = axes[a]; }
        if (moved) g_last_pad_ms = now;
    }
    pthread_mutex_unlock(&g_lock);
}
static void stick(float x, float y, int *ox, int *oy) {          /* radial dead zone, rescaled; y up positive */
    float m = sqrtf(x * x + y * y), dz = g_cfg.deadzone;
    if (m <= dz) { *ox = *oy = 0; return; }
    float k = (m > 1 ? 1 : m - dz) / (1 - dz) / m;
    if (m > 1) k = 1 / m;
    *ox = (int)(x * k * STICK_MAX); *oy = (int)(-y * k * STICK_MAX);
}
static void ctl_state(Ctl *c, NnPadState *o) {             /* g_lock held */
    o->buttons |= c->buttons | c->latched;         /* latched: a press released before this poll still counts once */
    c->latched = 0;
    int lx, ly, rx, ry;
    stick(c->ax[AMOTION_EVENT_AXIS_X], c->ax[AMOTION_EVENT_AXIS_Y], &lx, &ly);
    float zx = c->ax[AMOTION_EVENT_AXIS_Z], zy = c->ax[AMOTION_EVENT_AXIS_RZ];
    if (fabsf(zx) < 0.01f && fabsf(zy) < 0.01f) { zx = c->ax[AMOTION_EVENT_AXIS_RX]; zy = c->ax[AMOTION_EVENT_AXIS_RY]; }
    stick(zx, zy, &rx, &ry);
    if (lx || ly) { o->lx = lx; o->ly = ly; }
    if (rx || ry) { o->rx = rx; o->ry = ry; }
    float lt = fmaxf(c->ax[AMOTION_EVENT_AXIS_LTRIGGER], c->ax[AMOTION_EVENT_AXIS_BRAKE]), rt = fmaxf(c->ax[AMOTION_EVENT_AXIS_RTRIGGER], c->ax[AMOTION_EVENT_AXIS_GAS]);
    if (lt > 0.33f) o->buttons |= 1ull << B_ZL;
    if (rt > 0.33f) o->buttons |= 1ull << B_ZR;
    float hx = c->ax[AMOTION_EVENT_AXIS_HAT_X], hy = c->ax[AMOTION_EVENT_AXIS_HAT_Y];
    if (hx < -0.5f) o->buttons |= 1ull << B_LEFT;
    if (hx > 0.5f) o->buttons |= 1ull << B_RIGHT;
    if (hy < -0.5f) o->buttons |= 1ull << B_UP;
    if (hy > 0.5f) o->buttons |= 1ull << B_DOWN;
}

/* ---------------------------------------------------------------- motion (player 1) */
void android_motion(const float *accel, const float *gyro) {
    pthread_mutex_lock(&g_lock);
    if (accel) memcpy(g_accel, accel, sizeof g_accel);
    if (gyro) memcpy(g_gyro, gyro, sizeof g_gyro);
    g_has_sensors = 1;
    pthread_mutex_unlock(&g_lock);
}
static void motion_state(NnPadState *o, int64_t now) {           /* g_lock held; the axes as nnpad_sdl.c gives them */
    float a[3] = {g_accel[0], g_accel[1], g_accel[2]}, g[3] = {g_gyro[0], g_gyro[1], g_gyro[2]};
    if (now < g_shake_until) {                        /* SHAKE: a swing of about 3 G along the screen's x axis, back and forth */
        float t = (float)(g_shake_until - now) / 120.0f;
        a[0] += (t > 0.5f ? 30.0f : -30.0f);
        g[2] += 12.0f;
    }
    o->has_motion = g_cfg.motion && (g_has_sensors || now < g_shake_until);
    o->accel[0] = -a[0] / 9.80665f; o->accel[1] = a[1] / 9.80665f; o->accel[2] = -a[2] / 9.80665f;
    o->gyro[0] = -g[0] / 6.2831853f; o->gyro[1] = g[1] / 6.2831853f; o->gyro[2] = -g[2] / 6.2831853f;
}

/* ---------------------------------------------------------------- config/controls.ini */
static const char k_default_ini[] =
"; Controls (Android). Written with these defaults at the first start; read again when it changes.\n"
"; layout: positional = the face button in the console's A position (right) acts as A (B bottom, X top, Y left);\n"
";         labels = the printed letter decides.\n"
"layout = positional\n"
"; touch: auto = shown until a controller is used, back at the next touch; on; off\n"
"touch = auto\n"
"touch_opacity = 0.45\n"
"touch_scale = 1.0\n"
"; controller sticks: the dead zone (0..0.5)\n"
"deadzone = 0.15\n"
"; the touch camera: stick = a floating right stick (as the left one); drag = the camera follows the finger\n"
"camera_touch = stick\n"
"; the touch camera's speed (0.2..4; with the stick, below 1 slows it)\n"
"camera_speed = 1.0\n"
"; the phone's own motion sensors as the console's (Cappy throws, motion aiming): on | off\n"
"motion = on\n";
static uint64_t g_ini_stamp;
static void load_config(int force) {
    char path[PLAT_MAX_PATH], line[256];
    uint64_t stamp = 0;
    snprintf(path, sizeof path, "%s/config/controls.ini", plat_data_root());
    if (!plat_file_stamp(path, NULL, &stamp)) {
        snprintf(line, sizeof line, "%s/config", plat_data_root());
        plat_mkdirs(line);
        FILE *f = fopen(path, "w");
        if (f) { fputs(k_default_ini, f); fclose(f); }
        plat_file_stamp(path, NULL, &stamp);
    }
    if (!force && stamp == g_ini_stamp) return;
    g_ini_stamp = stamp;
    TButton btn[NBUTTONS];
    float st[2][3];
    int bind[B_DOWN + 1] = {0}, moved = 0, bound = 0, exit_key = AKEYCODE_BUTTON_MODE;
    memcpy(btn, k_buttons, sizeof btn);
    memcpy(st, k_stick, sizeof st);
    FILE *f = fopen(path, "r");
    while (f && fgets(line, sizeof line, f)) {
        char key[64], val[64];
        float x, y, size = 1;
        if (line[0] != ';' && sscanf(line, " pos_%63[^= ] = %f %f %f", key, &x, &y, &size) >= 3) {      /* the layout */
            x = x < 0 ? 0 : x > 1 ? 1 : x; y = y < 0 ? 0 : y > 1 ? 1 : y; size = size < 0.5f ? 0.5f : size > 2.5f ? 2.5f : size;
            if (!strcmp(key, "LSTICK") || !strcmp(key, "RSTICK")) { float *p = st[key[0] == 'R']; p[0] = x; p[1] = y; p[2] = size; moved++; }
            else for (int i = 0; i < NBUTTONS; i++) if (!strcmp(key, k_buttons[i].key)) { btn[i].x = x; btn[i].y = y; btn[i].r = k_buttons[i].r * size; moved++; }
            continue;
        }
        if (line[0] == ';' || sscanf(line, " %63[^= ] = %63s", key, val) != 2) continue;
        if (!strncmp(key, "bind_", 5)) {               /* the controller's keys */
            int code = atoi(val);
            for (int i = 0; i < NBUTTONS; i++) if (!strcmp(key + 5, k_buttons[i].key) && k_buttons[i].bit <= B_DOWN && code > 0 && code < NKEYS) { bind[k_buttons[i].bit] = code; bound++; }
            if (!strcmp(key + 5, "LS") && code > 0 && code < NKEYS) { bind[B_LS] = code; bound++; }
            if (!strcmp(key + 5, "EXIT") && code > 0 && code < NKEYS) { exit_key = code; bound++; }
            continue;
        }
        if (!strcmp(key, "layout")) g_cfg.layout_labels = !strcmp(val, "labels");
        else if (!strcmp(key, "touch")) g_cfg.touch_mode = !strcmp(val, "on") ? 1 : !strcmp(val, "off") ? 2 : 0;
        else if (!strcmp(key, "touch_opacity")) { float v = (float)atof(val); g_cfg.opacity = v < 0 ? 0 : v > 1 ? 1 : v; }
        else if (!strcmp(key, "touch_scale")) { float v = (float)atof(val); g_cfg.scale = v < 0.6f ? 0.6f : v > 1.6f ? 1.6f : v; }
        else if (!strcmp(key, "deadzone")) { float v = (float)atof(val); g_cfg.deadzone = v < 0 ? 0 : v > 0.5f ? 0.5f : v; }
        else if (!strcmp(key, "camera_speed")) { float v = (float)atof(val); g_cfg.camera = v < 0.2f ? 0.2f : v > 4 ? 4 : v; }
        else if (!strcmp(key, "motion")) g_cfg.motion = strcmp(val, "off") != 0;
        else if (!strcmp(key, "camera_touch")) g_cfg.camera_stick = strcmp(val, "drag") != 0;
    }
    if (f) fclose(f);
    pthread_mutex_lock(&g_lock);
    memcpy(g_btn, btn, sizeof g_btn);
    g_exit_key = exit_key;
    memcpy(g_stick, st, sizeof g_stick);
    memcpy(g_bind, bind, sizeof g_bind);
    pthread_mutex_unlock(&g_lock);
    if (moved || bound) printf("nnpad: controls: %d touch controls placed by the player, %d controller keys bound by the player\n", moved, bound);
    printf("nnpad: controls: layout %s, touch %s (opacity %.2f, scale %.2f), dead zone %.2f, camera %s speed %.2f, motion %s\n",
           g_cfg.layout_labels ? "labels" : "positional", g_cfg.touch_mode == 1 ? "on" : g_cfg.touch_mode == 2 ? "off" : "auto",
           g_cfg.opacity, g_cfg.scale, g_cfg.deadzone, g_cfg.camera_stick ? "stick" : "drag", g_cfg.camera, g_cfg.motion ? "on" : "off");
}

static void save_touch_mode(void) {               /* g_lock held: the touch line of controls.ini, the rest as it is */
    char path[PLAT_MAX_PATH], line[256];
    static char text[16384];
    size_t n = 0;
    snprintf(path, sizeof path, "%s/config/controls.ini", plat_data_root());
    FILE *f = fopen(path, "r");
    int done = 0;
    while (f && fgets(line, sizeof line, f)) {
        char key[64], val[64];
        if (line[0] != ';' && sscanf(line, " %63[^= ] = %63s", key, val) == 2 && !strcmp(key, "touch")) { snprintf(line, sizeof line, "touch = %s\n", g_cfg.touch_mode == 2 ? "off" : g_cfg.touch_mode == 1 ? "on" : "auto"); done = 1; }
        size_t l = strlen(line);
        if (n + l < sizeof text) { memcpy(text + n, line, l); n += l; }
    }
    if (f) fclose(f);
    if (!done) n += (size_t)snprintf(text + n, sizeof text - n, "touch = %s\n", g_cfg.touch_mode == 2 ? "off" : "auto");
    f = fopen(path, "w");
    if (f) { fwrite(text, 1, n, f); fclose(f); }
    plat_file_stamp(path, NULL, &g_ini_stamp);          /* not read again for this change */
    printf("nnpad: touch controls %s (the ring at the top)\n", g_cfg.touch_mode == 2 ? "off" : "on");
}

/* ---------------------------------------------------------------- nnpad.h */
int nnpad_init(void) { load_config(1); return 1; }
void nnpad_poll(int force) {
    static uint64_t last;
    uint64_t now = plat_ms64();
    if (!force && now - last < 1000) return;
    last = now;
    load_config(0);
}
int nnpad_present(int player) {
    if (player == 0) return 1;                        /* the touch pad is always there */
    pthread_mutex_lock(&g_lock);
    int any = 0;
    for (int i = 0; i < 8; i++) any |= g_ctl[i].used && g_ctl[i].player == player;
    pthread_mutex_unlock(&g_lock);
    return any;
}
int nnpad_get(int player, NnPadState *out) {
    int64_t now = (int64_t)plat_ms64();
    int any = player == 0;
    memset(out, 0, sizeof *out);
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < 8; i++) if (g_ctl[i].used && g_ctl[i].player == player) { ctl_state(&g_ctl[i], out); any = 1; }
    if (player == 0) {
        touch_state(out, now);
        out->buttons |= g_back_key;
        motion_state(out, now);
    }
    pthread_mutex_unlock(&g_lock);
    out->buttons &= 0xffff;
    out->wired = 1;
    {   /* SMO_PADLOG=1: player 1's sticks and buttons, at most once a second while something is held */
        static int on = -1; static int64_t last;
        if (on < 0) on = getenv("SMO_PADLOG") && atoi(getenv("SMO_PADLOG"));
        if (on && player == 0 && (out->lx || out->ly || out->rx || out->ry || out->buttons) && now - last >= 1000) {
            last = now;
            printf("nnpad: player 1: buttons %llx, left stick %d %d, right stick %d %d\n", (unsigned long long)out->buttons, out->lx, out->ly, out->rx, out->ry);
        }
    }
    return any;
}
void nnpad_rumble(int player, float low, float high) { (void)player; (void)low; (void)high; }    /* TODO: Vibrator / InputDevice vibration through JNI */
void nnpad_disconnect(int player) { (void)player; }
void nnpad_swap(int a, int b) {
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < 8; i++) if (g_ctl[i].used) { if (g_ctl[i].player == a) g_ctl[i].player = b; else if (g_ctl[i].player == b) g_ctl[i].player = a; }
    pthread_mutex_unlock(&g_lock);
}
int nnpad_keyboard_player(void) { return 0; }
/* a hardware keyboard: the keys of nnhid.c's own table (Z X C V = A B X Y, Q E = L R, A S = ZL ZR, Enter = +, Backspace
 * = -, I J K L = d-pad, arrows = left stick, 8 4 6 2 = right stick) */
int nnpad_kb_vk(int idx) {
    static const int k[24] = {'Z', 'X', 'C', 'V', 0, 0, 'Q', 'E', 'A', 'S', PLAT_VK_RETURN, PLAT_VK_BACK, 'J', 'I', 'L', 'K',
                              PLAT_VK_LEFT, PLAT_VK_UP, PLAT_VK_RIGHT, PLAT_VK_DOWN, '4', '8', '6', '2'};
    return idx >= 0 && idx < 24 ? k[idx] : 0;
}
int nnpad_hotkey_vk(int which) { return which == 0 ? PLAT_VK_F1 : 0; }
unsigned nnpad_hotkey_combo(void) { return (1u << B_LS) | (1u << B_RS); }     /* both stick clicks: the amiibo menu */
int nnpad_tool(int argc, char **argv) { (void)argc; (void)argv; return -1; }
int nnpad_selftest(const NnHidHooks *h) { (void)h; printf("nnpad: the SDL pad self-test is a desktop test\n"); return 0; }
