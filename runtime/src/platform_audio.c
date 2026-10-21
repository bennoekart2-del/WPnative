/* Host audio output: the AI DMA stream (what the DSP mixed into main memory) played through an
 * SDL3 audio stream. The guest data is s16 stereo, big endian, right sample first; SDL gets it
 * as native-endian s16 with left first, at the AI DMA rate (32 or 48 kHz).
 *
 * WPR_DUMP_AUDIO=<file.wav> also writes everything played to a WAV file (its header is kept up
 * to date as it grows, so the file is usable even if the process is killed).
 * audio_play_ai() is called from the timer thread. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

static int ready;
static SDL_AudioStream* stream;
static uint32_t stream_rate;
static FILE* wav;
static uint32_t wav_rate, wav_bytes, wav_writes;

static void put_le(uint8_t* p, uint32_t v, int n) {
    for (int i = 0; i < n; i++) p[i] = (uint8_t)(v >> (8 * i));
}

static void wav_header(void) {
    uint8_t h[44];
    memcpy(h, "RIFF", 4); put_le(h + 4, 36 + wav_bytes, 4); memcpy(h + 8, "WAVEfmt ", 8);
    put_le(h + 16, 16, 4); put_le(h + 20, 1, 2); put_le(h + 22, 2, 2);  /* PCM, stereo */
    put_le(h + 24, wav_rate, 4); put_le(h + 28, wav_rate * 4, 4);
    put_le(h + 32, 4, 2); put_le(h + 34, 16, 2);
    memcpy(h + 36, "data", 4); put_le(h + 40, wav_bytes, 4);
    long at = ftell(wav);
    fseek(wav, 0, SEEK_SET);
    fwrite(h, 1, sizeof h, wav);
    if (at > 0) fseek(wav, at, SEEK_SET);
    fflush(wav);
}

static void audio_open(uint32_t rate) {
    ready = 1;
    const char* dump = getenv("WPR_DUMP_AUDIO");
    if (dump && *dump) {
        wav = fopen(dump, "wb");
        if (wav) { wav_rate = rate; wav_header(); }
        else fprintf(stderr, "[audio] cannot write %s\n", dump);
    }
    if (!SDL_WasInit(SDL_INIT_AUDIO) && !SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        fprintf(stderr, "[audio] SDL audio unavailable: %s\n", SDL_GetError());
        return;
    }
    SDL_AudioSpec spec = {SDL_AUDIO_S16, 2, (int)rate};
    stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (!stream) {
        fprintf(stderr, "[audio] no audio device: %s\n", SDL_GetError());
        return;
    }
    stream_rate = rate;
    SDL_ResumeAudioStreamDevice(stream);
}

void audio_play_ai(const uint8_t* be, uint32_t bytes, uint32_t rate) {
    static int16_t buf[0x8000 * 32 / 2];
    if (!ready) audio_open(rate);
    bytes &= ~3u;
    if (bytes > sizeof buf) bytes = sizeof buf;
    uint32_t frames = bytes / 4;
    for (uint32_t i = 0; i < frames; i++) {
        const uint8_t* s = be + 4 * i;
        buf[2 * i] = (int16_t)(s[2] << 8 | s[3]);      /* left */
        buf[2 * i + 1] = (int16_t)(s[0] << 8 | s[1]);  /* right */
    }
    if (stream) {
        if (rate != stream_rate) {
            SDL_AudioSpec spec = {SDL_AUDIO_S16, 2, (int)rate};
            SDL_SetAudioStreamFormat(stream, &spec, NULL);
            stream_rate = rate;
        }
        /* keep the latency bounded (about 150 ms) if the device drains slower than we feed */
        if (SDL_GetAudioStreamQueued(stream) < (int)(rate * 4 * 3 / 20))
            SDL_PutAudioStreamData(stream, buf, (int)bytes);
    }
    if (wav) {
        fwrite(buf, 1, bytes, wav);  /* WAV is little endian: true of every host we build for */
        wav_bytes += bytes;
        if ((++wav_writes & 63) == 0) wav_header();
    }
}
