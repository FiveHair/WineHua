/*
 * WineHua Audio Test - Common utilities
 *
 * Shared infrastructure for all test modules.
 * Each test module (test_wasapi.c, test_dsound.c, etc.) receives a LogFn
 * callback to output results.  The GUI (main.c) hooks this callback to
 * append text to the log Edit control.
 */

#ifndef WINEHUA_AUDIO_TEST_COMMON_H
#define WINEHUA_AUDIO_TEST_COMMON_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -- Logging callback ------------------------------------------------ */

typedef void (*LogFn)(const char *msg);

/* Helper to format and deliver a log line.  Thread-safe: allocates a
 * temporary buffer on the stack (up to 2 KiB) then calls `log`. */
static inline void test_log(LogFn log, const char *fmt, ...)
{
    char buf[2048];
    va_list va;
    va_start(va, fmt);
    vsnprintf(buf, sizeof(buf), fmt, va);
    va_end(va);
    if (log) log(buf);
}

#define LOG_PASS(log, msg)    test_log(log, "[PASS] " msg)
#define LOG_PASS_F(log, ...)  test_log(log, "[PASS] " __VA_ARGS__)
#define LOG_FAIL(log, msg, hr)                                    \
    do {                                                          \
        char _buf[256];                                           \
        snprintf(_buf, sizeof(_buf), "[FAIL] %s (0x%08lx)",       \
                 msg, (unsigned long)(hr));                        \
        if (log) log(_buf);                                       \
    } while (0)
#define LOG_FAIL_F(log, ...)                                      \
    do {                                                          \
        char _buf[512];                                           \
        snprintf(_buf, sizeof(_buf), "[FAIL] " __VA_ARGS__);      \
        if (log) log(_buf);                                       \
    } while (0)
#define LOG_INFO(log, msg)   test_log(log, "[INFO] " msg)
#define LOG_INFO_F(log, ...) test_log(log, "[INFO] " __VA_ARGS__)

/* -- HRESULT name lookup -------------------------------------------- */

static inline const char *hr_name(HRESULT hr)
{
    switch (hr) {
    case S_OK:                              return "S_OK";
    case S_FALSE:                            return "S_FALSE";
    case E_OUTOFMEMORY:                      return "E_OUTOFMEMORY";
    case E_INVALIDARG:                        return "E_INVALIDARG";
    case E_POINTER:                          return "E_POINTER";
    case E_NOTIMPL:                          return "E_NOTIMPL";
    case E_ACCESSDENIED:                     return "E_ACCESSDENIED";
    case E_FAIL:                             return "E_FAIL";
    case HRESULT_FROM_WIN32(ERROR_TIMEOUT):  return "WAIT_TIMEOUT";
    default: return "";
    }
}

/* -- PCM tone generator ----------------------------------------------
 * Generates `durationSec` seconds of a sine wave at `freq` Hz.
 * Returns heap-allocated s16 interleaved PCM, or NULL on failure.
 * Caller must free().  */

static inline short *generate_tone_s16(DWORD *outFrames,
                                        DWORD sampleRate,
                                        WORD  channels,
                                        float freq,
                                        float durationSec)
{
    DWORD frames = (DWORD)(sampleRate * durationSec);
    if (!frames) frames = sampleRate; /* default 1s */
    short *pcm = (short *)calloc(frames * channels, sizeof(short));
    if (!pcm) return NULL;

    for (DWORD i = 0; i < frames; i++) {
        float t = (float)i / (float)sampleRate;
        float val = sinf(2.0f * 3.14159265f * freq * t) * 0.3f; /* -0.3..+0.3 */
        short s = (short)(val * 32767.0f);
        for (WORD ch = 0; ch < channels; ch++)
            pcm[i * channels + ch] = s;
    }

    *outFrames = frames;
    return pcm;
}

/* -- WAV file writer ------------------------------------------------- */

static inline BOOL generate_wav_file(const char *path,
                                      DWORD sampleRate,
                                      WORD  channels,
                                      WORD  bitsPerSample,
                                      float freq,
                                      float durationSec)
{
    DWORD frames;
    short *pcm = generate_tone_s16(&frames, sampleRate, channels, freq, durationSec);
    if (!pcm) return FALSE;

    WORD  blockAlign = channels * (bitsPerSample / 8);
    DWORD byteRate   = sampleRate * blockAlign;
    DWORD dataSize   = frames * blockAlign;
    DWORD fileSize   = 36 + dataSize;

    FILE *f = fopen(path, "wb");
    if (!f) { free(pcm); return FALSE; }

    /* RIFF header */
    fwrite("RIFF", 1, 4, f);
    fwrite(&fileSize, 4, 1, f);
    fwrite("WAVE", 1, 4, f);

    /* fmt chunk */
    WORD  audioFormat = 1; /* PCM */
    DWORD fmtSize     = 16;
    fwrite("fmt ", 1, 4, f);
    fwrite(&fmtSize, 4, 1, f);
    fwrite(&audioFormat, 2, 1, f);
    fwrite(&channels, 2, 1, f);
    fwrite(&sampleRate, 4, 1, f);
    fwrite(&byteRate, 4, 1, f);
    fwrite(&blockAlign, 2, 1, f);
    fwrite(&bitsPerSample, 2, 1, f);

    /* data chunk */
    fwrite("data", 1, 4, f);
    fwrite(&dataSize, 4, 1, f);

    /* upconvert to requested bits (only 8/16 supported) */
    if (bitsPerSample == 16) {
        fwrite(pcm, 2, frames * channels, f);
    } else if (bitsPerSample == 8) {
        BYTE *u8 = (BYTE *)malloc(frames * channels);
        if (u8) {
            for (DWORD i = 0; i < frames * channels; i++)
                u8[i] = (BYTE)((pcm[i] >> 8) + 128);
            fwrite(u8, 1, frames * channels, f);
            free(u8);
        }
    }

    fclose(f);
    free(pcm);
    return TRUE;
}

#ifdef __cplusplus
}
#endif

#endif /* WINEHUA_AUDIO_TEST_COMMON_H */
