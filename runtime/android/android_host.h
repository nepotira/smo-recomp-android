/* android_host.h - what the Android host files tell each other (android_main.c, nnpad_android.c, the renderer). */
#ifndef ANDROID_HOST_H
#define ANDROID_HOST_H

/* android_main.c -> nnpad_android.c, on the activity's thread */
#define ANDROID_PAD_BACK_GESTURE (-0x7ffffff0)    /* android_pad_key's device for the touch pad's back gesture (-1 is a real id: virtual devices, injected input) */
void android_exit_request(void);      /* android_main.c: the player asked to leave the game (the touch EXIT button, bind_EXIT): GameActivity asks */
int android_pad_key(int device, int keycode, int down);         /* a controller's key (device ANDROID_PAD_BACK_GESTURE: the touch pad's back gesture); 1 = used */
void android_pad_axes(int device, const float *axes, int count); /* AMOTION_EVENT_AXIS_* values of a controller */
void android_touch(int pointer, float x, float y, int down);    /* a finger, x and y 0..1 of the window */
void android_motion(const float *accel, const float *gyro);     /* m/s^2 and rad/s in the screen's axes (x right, y up, z out); either NULL */

/* android_main.c -> the renderer (nvn_vk.c): the window was replaced (NULL: it goes away now; the call returns once the
 * renderer no longer uses the old one) */
void android_window_changed(void *window);
int android_window_generation(void);
void *android_window_current(void);

/* nnpad_android.c -> the renderer: the touch controls to draw (normalised window coordinates) */
typedef struct { float x, y, r; int round, pressed; char label[4]; } TouchShape;
int android_touch_shapes(TouchShape *out, int max, float *opacity);   /* 0 when nothing is to be drawn */
void android_touch_aspect(float aspect);                              /* the renderer: width / height of the window */
#endif
