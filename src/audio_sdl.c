/*
 * SDL2 output device for the standalone app. The mixer in audio.c knows
 * nothing about SDL, so plugin builds link the engine on its own.
 */
#include "SDL.h"

#include <stdio.h>
#include <stdlib.h>

#include "audio.h"

#define DEVICE_RATE 48000


struct audio {
    mixer_t *m;
    SDL_AudioDeviceID dev;
};

static void sdl_audio_cb(void *user, Uint8 *stream, int len)
{
    mixer_render((mixer_t *)user, (float *)stream, len / (int)(sizeof(float) * 2));
}

audio_t *audio_open(mixer_t *m)
{
    if (!m)
        return NULL;
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "audio: SDL audio init failed: %s\n", SDL_GetError());
        return NULL;
    }

    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq = DEVICE_RATE;
    want.format = AUDIO_F32SYS;
    want.channels = 2;
    want.samples = 512;
    want.callback = sdl_audio_cb;
    want.userdata = m;

    SDL_AudioDeviceID dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!dev) {
        fprintf(stderr, "audio: no output device: %s\n", SDL_GetError());
        return NULL;
    }

    audio_t *a = calloc(1, sizeof *a);
    if (!a) {
        SDL_CloseAudioDevice(dev);
        return NULL;
    }
    a->m = m;
    a->dev = dev;
    SDL_PauseAudioDevice(dev, 0);
    fprintf(stderr, "audio: %d Hz, %d ch, %d frames/block, driver %s\n", have.freq,
            have.channels, have.samples, SDL_GetCurrentAudioDriver());
    return a;
}

void audio_close(audio_t *a)
{
    if (!a)
        return;
    SDL_PauseAudioDevice(a->dev, 1);
    SDL_CloseAudioDevice(a->dev);
    free(a);
}

int audio_active(const audio_t *a)
{
    return a && a->dev;
}
