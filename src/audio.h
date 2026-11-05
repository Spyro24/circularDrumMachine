#ifndef CDM_AUDIO_H
#define CDM_AUDIO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Sample engine. The mixer owns the transport clock: it renders from a snapshot
 * of the kit pushed by the UI, so hits land on the exact frame rather than on
 * whatever the 60 Hz redraw happened to catch. The same mixer renders offline,
 * which is how the timing is tested and how a VST3 build will pull audio from
 * the host callback.
 */

#define AUDIO_TRACKS 8

/*
 * Hits sit at phase + k*div and wrap past the loop point. A KILL starting on a
 * hit's tick cancels it; with full set, a KILL starting anywhere inside the
 * hit's own step cancels it too.
 */
typedef struct {
    int div_ticks;  /* ticks between hits of this track */
    int kill_ticks;     /* where KILL hits start */
    int kill_off_ticks; /* phase of the kill grid: 0 .. kill_ticks - 1 */
    int off_ticks;      /* phase of the grid inside the loop: 0 .. div_ticks - 1 */
    int full;       /* KILL covers the hit's whole step, not just its tick */
    float gain;     /* linear track level; 0 is -inf dB, 1 is unity (0 dB) */
} audio_track_cfg;

typedef struct {
    int loop_ticks;     /* bars * ticks per bar */
    int ticks_per_beat; /* 1/96 notes: 24 per quarter */
    double bpm;
    audio_track_cfg tr[AUDIO_TRACKS];
} audio_kit;

/* ---------------------------------------------------------------- mixer --- */

typedef struct mixer mixer_t;

mixer_t *mixer_new(int rate);
void mixer_free(mixer_t *m);

/* Copy a kit snapshot; safe from another thread. */
void mixer_set_kit(mixer_t *m, const audio_kit *k);
void mixer_play(mixer_t *m, int playing);
int mixer_playing(mixer_t *m);
/* Position inside the loop, in ticks (0 .. loop_ticks). */
double mixer_pos_ticks(mixer_t *m);

/* Audition a track's sample once, from the top. */
void mixer_trigger(mixer_t *m, int track);

/* Decode path into the track; returns 0 on success, -1 if it cannot be read. */
int mixer_load(mixer_t *m, int track, const char *path);
void mixer_unload(mixer_t *m, int track);

/* Interleaved stereo float, exactly what the device callback asks for. */
void mixer_render(mixer_t *m, float *out, int frames);

/* Decode a .wav/.mp3 to mono float at its own sample rate. Caller frees *out. */
int audio_decode(const char *path, float **out, int *frames, int *rate);

/* ----------------------------------------------------------- SDL device --- */

typedef struct audio audio_t;

/* Opens the default device for m's rate. NULL when there is no output. */
audio_t *audio_open(mixer_t *m);
void audio_close(audio_t *a);
int audio_active(const audio_t *a);

#ifdef __cplusplus
}
#endif

#endif
