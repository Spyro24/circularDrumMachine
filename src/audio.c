#define DR_WAV_IMPLEMENTATION
#define DR_MP3_IMPLEMENTATION
#include "dr_wav.h"
#include "dr_mp3.h"

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio.h"

#define ENGINE_RATE 48000
#define VOICE_GAIN 0.75f

typedef struct {
    float *data;
    int frames;
    int rate;
} sample_t;

typedef struct {
    const float *data; /* NULL when idle */
    int frames;
    double pos;
    double inc;
    float gain;
} voice_t;

struct mixer {
    int rate;
    pthread_mutex_t lock;
    audio_kit kit;
    int has_kit;
    sample_t smp[AUDIO_TRACKS];
    voice_t vc[AUDIO_TRACKS];
    double pos; /* ticks, unwrapped */
    int playing;
};

/* --------------------------------------------------------------- decoding */

static int has_ext(const char *path, const char *ext)
{
    size_t a = strlen(path), b = strlen(ext);
    return a >= b && strcasecmp(path + a - b, ext) == 0;
}

static float *downmix(const float *inter, int frames, int ch)
{
    if (frames <= 0 || ch <= 0)
        return NULL;
    float *mono = malloc(sizeof(float) * (size_t)frames);
    if (!mono)
        return NULL;
    for (int i = 0; i < frames; i++) {
        float s = 0.0f;
        for (int c = 0; c < ch; c++)
            s += inter[(size_t)i * (size_t)ch + (size_t)c];
        mono[i] = s / (float)ch;
    }
    return mono;
}

int audio_decode(const char *path, float **out, int *frames_out, int *rate_out)
{
    float *inter = NULL;
    int frames = 0, rate = 0, ch = 1;

    if (has_ext(path, ".mp3")) {
        drmp3 mp;
        if (!drmp3_init_file(&mp, path, NULL))
            return -1;
        drmp3_uint64 n = drmp3_get_pcm_frame_count(&mp);
        if (n == 0) {
            drmp3_uninit(&mp);
            return -1;
        }
        inter = malloc(sizeof(float) * (size_t)n * (size_t)mp.channels);
        if (!inter) {
            drmp3_uninit(&mp);
            return -1;
        }
        drmp3_uint64 got = drmp3_read_pcm_frames_f32(&mp, n, inter);
        rate = (int)mp.sampleRate;
        ch = (int)mp.channels;
        frames = (int)got;
        drmp3_uninit(&mp);
    } else {
        drwav w;
        if (!drwav_init_file(&w, path, NULL))
            return -1;
        drwav_uint64 n = w.totalPCMFrameCount;
        if (n == 0) {
            drwav_uninit(&w);
            return -1;
        }
        inter = malloc(sizeof(float) * (size_t)n * (size_t)w.channels);
        if (!inter) {
            drwav_uninit(&w);
            return -1;
        }
        drwav_uint64 got = drwav_read_pcm_frames_f32(&w, n, inter);
        rate = (int)w.sampleRate;
        ch = (int)w.channels;
        frames = (int)got;
        drwav_uninit(&w);
    }

    if (!inter || frames <= 0 || rate <= 0) {
        free(inter);
        return -1;
    }
    float *mono = downmix(inter, frames, ch);
    free(inter);
    if (!mono)
        return -1;

    *out = mono;
    *frames_out = frames;
    *rate_out = rate;
    return 0;
}

/* ------------------------------------------------------------------ mixer */

mixer_t *mixer_new(int rate)
{
    mixer_t *m = calloc(1, sizeof *m);
    if (!m)
        return NULL;
    m->rate = rate > 0 ? rate : ENGINE_RATE;
    pthread_mutex_init(&m->lock, NULL);
    return m;
}

void mixer_free(mixer_t *m)
{
    if (!m)
        return;
    pthread_mutex_destroy(&m->lock);
    for (int i = 0; i < AUDIO_TRACKS; i++)
        free(m->smp[i].data);
    free(m);
}

void mixer_set_kit(mixer_t *m, const audio_kit *k)
{
    if (!m || !k)
        return;
    pthread_mutex_lock(&m->lock);
    m->kit = *k;
    m->has_kit = k->loop_ticks > 0;
    pthread_mutex_unlock(&m->lock);
}

static double samples_per_tick(const mixer_t *m)
{
    double tpb = m->kit.ticks_per_beat > 0 ? m->kit.ticks_per_beat : 24.0;
    double bpm = m->kit.bpm > 1.0 ? m->kit.bpm : 120.0;
    double spt = (double)m->rate * 60.0 / (bpm * tpb);
    return spt < 1.0 ? 1.0 : spt;
}

static void trigger_voice(mixer_t *m, int track, float gain)
{
    sample_t *s = &m->smp[track];
    if (!s->data || s->frames < 2 || gain <= 0.0f)
        return; /* a track pulled to -inf dB makes no sound at all */
    voice_t *x = &m->vc[track];
    x->data = s->data;
    x->frames = s->frames;
    x->pos = 0.0;
    x->inc = (double)s->rate / (double)m->rate;
    x->gain = gain * VOICE_GAIN;
}

/*
 * A hit fires wherever the DIV grid sits, i.e. on every tick with the track's
 * phase. The grid repeats across the loop point, so an offset turns it instead
 * of leaving a silent slice at the start. A KILL cancels a hit when a kill
 * starts on that same tick, or -- with full -- anywhere inside the hit's own
 * step. The kill grid has its own phase, so it can sit off the beat. Same rule
 * the rings draw.
 */
static void fire_tick(mixer_t *m, int tick)
{
    for (int v = 0; v < AUDIO_TRACKS; v++) {
        const audio_track_cfg *t = &m->kit.tr[v];
        if (t->div_ticks <= 0)
            continue;
        if (tick % t->div_ticks != t->off_ticks)
            continue;
        if (t->kill_ticks > 0) {
            int kt = t->kill_ticks;
            int kp = t->kill_off_ticks % kt;
            int killed = ((tick % kt) + kt - kp) % kt == 0;
            if (!killed && t->full) {
                int last = tick + t->div_ticks - 1;
                int next = tick + (((kp - tick) % kt) + kt) % kt;
                killed = next <= last;
            }
            if (killed)
                continue;
        }
        trigger_voice(m, v, t->gain);
    }
}

static void render_voices(mixer_t *m, float *out, int frames)
{
    for (int v = 0; v < AUDIO_TRACKS; v++) {
        voice_t *x = &m->vc[v];
        if (!x->data)
            continue;
        for (int i = 0; i < frames; i++) {
            int i0 = (int)x->pos;
            if (i0 < 0 || i0 >= x->frames - 1) {
                x->data = NULL;
                break;
            }
            double f = x->pos - (double)i0;
            float s = (float)((double)x->data[i0] * (1.0 - f) + (double)x->data[i0 + 1] * f);
            out[(size_t)i * 2] += s * x->gain;
            out[(size_t)i * 2 + 1] += s * x->gain;
            x->pos += x->inc;
        }
    }
}

static void advance_transport(mixer_t *m, int frames)
{
    int loop = m->kit.loop_ticks > 0 ? m->kit.loop_ticks : 1;
    double pos1 = m->pos + (double)frames / samples_per_tick(m);
    long long t0 = (long long)floor(m->pos);
    long long t1 = (long long)floor(pos1);
    for (long long t = t0 + 1; t <= t1; t++)
        fire_tick(m, (int)(t % loop));
    m->pos = pos1;
}

void mixer_render(mixer_t *m, float *out, int frames)
{
    if (!m || !out || frames <= 0)
        return;

    pthread_mutex_lock(&m->lock);
    memset(out, 0, sizeof(float) * 2 * (size_t)frames);

    float *base = out;
    int left = frames;
    while (left > 0) {
        int chunk = left;
        if (m->playing && m->has_kit) {
            /* Cut the block where the next tick starts so every hit triggers
             * on the frame it belongs to. */
            double frac = m->pos - floor(m->pos);
            double to_next = (1.0 - frac) * samples_per_tick(m);
            int c = (int)ceil(to_next - 1e-9);
            if (c >= 1 && c < chunk)
                chunk = c;
        }
        render_voices(m, out, chunk);
        if (m->playing && m->has_kit)
            advance_transport(m, chunk);
        out += (size_t)chunk * 2;
        left -= chunk;
    }

    for (int i = 0; i < frames * 2; i++) {
        float s = base[i];
        if (s > 1.0f)
            s = 1.0f;
        else if (s < -1.0f)
            s = -1.0f;
        base[i] = s;
    }
    pthread_mutex_unlock(&m->lock);
}

void mixer_play(mixer_t *m, int playing)
{
    if (!m)
        return;
    pthread_mutex_lock(&m->lock);
    if (playing && !m->playing) {
        m->pos = 0.0;
        m->playing = 1;
        if (m->has_kit)
            fire_tick(m, 0); /* the downbeat belongs to the loop */
    } else if (!playing) {
        m->playing = 0;
        m->pos = 0.0;
        /* Stop silences tails too, including a pad still ringing. */
        for (int v = 0; v < AUDIO_TRACKS; v++)
            m->vc[v].data = NULL;
    }
    pthread_mutex_unlock(&m->lock);
}

int mixer_playing(mixer_t *m)
{
    if (!m)
        return 0;
    pthread_mutex_lock(&m->lock);
    int p = m->playing;
    pthread_mutex_unlock(&m->lock);
    return p;
}

double mixer_pos_ticks(mixer_t *m)
{
    if (!m)
        return 0.0;
    pthread_mutex_lock(&m->lock);
    double p = m->pos;
    int loop = m->kit.loop_ticks;
    if (loop > 0)
        p = fmod(p, (double)loop);
    pthread_mutex_unlock(&m->lock);
    return p;
}

void mixer_trigger(mixer_t *m, int track)
{
    if (!m || track < 0 || track >= AUDIO_TRACKS)
        return;
    pthread_mutex_lock(&m->lock);
    trigger_voice(m, track, m->has_kit ? m->kit.tr[track].gain : 1.0f);
    pthread_mutex_unlock(&m->lock);
}

int mixer_load(mixer_t *m, int track, const char *path)
{
    if (!m || !path || track < 0 || track >= AUDIO_TRACKS)
        return -1;

    float *data = NULL;
    int frames = 0, rate = 0;
    if (audio_decode(path, &data, &frames, &rate) != 0)
        return -1;

    pthread_mutex_lock(&m->lock);
    free(m->smp[track].data);
    m->smp[track].data = data;
    m->smp[track].frames = frames;
    m->smp[track].rate = rate;
    m->vc[track].data = NULL;
    pthread_mutex_unlock(&m->lock);
    return 0;
}

void mixer_unload(mixer_t *m, int track)
{
    if (!m || track < 0 || track >= AUDIO_TRACKS)
        return;
    pthread_mutex_lock(&m->lock);
    free(m->smp[track].data);
    m->smp[track].data = NULL;
    m->smp[track].frames = 0;
    m->vc[track].data = NULL;
    pthread_mutex_unlock(&m->lock);
}
