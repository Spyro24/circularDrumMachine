#ifndef CDM_UI_H
#define CDM_UI_H

#include "audio.h"
#include "draw.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ui ui_t;

enum { UI_DOWN = 0, UI_UP = 1, UI_MOVE = 2 };

ui_t *ui_create(void);
void ui_destroy(ui_t *u);

/* Draw the whole interface into c. Clears the dirty flag. */
void ui_frame(ui_t *u, canvas_t *c);
/* Advance the transport / animations. */
void ui_tick(ui_t *u, float dt);

int ui_needs_frame(const ui_t *u);
int ui_animating(const ui_t *u);

/* All three return 1 when something changed and a redraw is needed. */
int ui_pointer(ui_t *u, int event, float x, float y, int button);
int ui_wheel(ui_t *u, float dy, float x, float y);
int ui_key(ui_t *u, int key, int down);
/* Printable characters from SDL_TEXTINPUT, swallowed unless a ratio field is
 * being edited. Returns 1 when a redraw is needed. */
int ui_text(ui_t *u, const char *text);
/* A .wav/.mp3 dropped at window position (x, y): assigns it to the sample slot
 * under the cursor. Returns 1 when a redraw is needed. */
int ui_drop(ui_t *u, float x, float y, const char *path);

/* Attach the sample engine; every pattern, rate and BPM change is pushed to
 * it, and the playhead follows its clock. */
void ui_set_mixer(ui_t *u, mixer_t *m);

/*
 * Plugin builds drive the transport from the host. ui_set_host_transport()
 * declares that a host owns the clock and reports whether it is rolling plus
 * its position in quarter notes; while it is rolling the play button is inert
 * and the rings follow the host. Standalone never calls this, so the internal
 * clock is free to run from the play button.
 */
void ui_set_host_transport(ui_t *u, int playing, double beats);
/* 1 when the sequencer is advancing, from either clock. */
int ui_transport_running(const ui_t *u);

#ifdef __cplusplus
}
#endif

#endif
