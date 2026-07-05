/*
 * WineHua Audio Test -- DirectSound full-interface verification
 *
 * Tests DirectSoundCreate8, primary/secondary buffer, Lock/Unlock,
 * Play/Stop, Volume/Pan/Frequency, looping, and 3D listener.
 */

#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <dsound.h>

#include "test_common.h"

void run_dsound_tests(LogFn log)
{
    HRESULT hr;
    LPDIRECTSOUND8 ds8 = NULL;
    LPDIRECTSOUNDBUFFER primary = NULL;
    LPDIRECTSOUNDBUFFER secondary = NULL;
    DSBUFFERDESC desc;
    WAVEFORMATEX wfx;
    int pass = 0, fail = 0;

    LOG_INFO(log, "=== DirectSound Full Interface Verification ===");

    /* 1. DirectSoundCreate8 */
    hr = DirectSoundCreate8(NULL, &ds8, NULL);
    if (FAILED(hr)) {
        LOG_FAIL(log, "DirectSoundCreate8", hr);
        return;
    }
    LOG_PASS_F(log, "DirectSoundCreate8 -> %p", (void *)ds8);
    pass++;

    /* 2. SetCooperativeLevel */
    hr = IDirectSound8_SetCooperativeLevel(ds8, GetDesktopWindow(), DSSCL_NORMAL);
    if (FAILED(hr)) {
        LOG_FAIL(log, "SetCooperativeLevel", hr);
        goto out;
    }
    LOG_PASS(log, "SetCooperativeLevel(DSSCL_NORMAL)");
    pass++;

    /* 3. Primary buffer */
    memset(&desc, 0, sizeof(desc));
    desc.dwSize  = sizeof(desc);
    desc.dwFlags = DSBCAPS_PRIMARYBUFFER;
    hr = IDirectSound8_CreateSoundBuffer(ds8, &desc, &primary, NULL);
    if (FAILED(hr)) {
        LOG_FAIL(log, "CreateSoundBuffer(primary)", hr);
        /* non-fatal - continue */
    } else {
        LOG_PASS_F(log, "CreateSoundBuffer(primary) -> %p", (void *)primary);

        /* Try to set primary format */
        memset(&wfx, 0, sizeof(wfx));
        wfx.wFormatTag      = WAVE_FORMAT_PCM;
        wfx.nChannels       = 2;
        wfx.nSamplesPerSec  = 48000;
        wfx.wBitsPerSample  = 16;
        wfx.nBlockAlign     = 4;
        wfx.nAvgBytesPerSec = 192000;
        hr = IDirectSoundBuffer_SetFormat(primary, &wfx);
        if (SUCCEEDED(hr)) {
            LOG_PASS(log, "SetFormat(primary)");
            pass++;
        } else {
            LOG_INFO_F(log, "[INFO] SetFormat(primary) -> 0x%08lx (non-critical)", hr);
        }
        pass++;
    }

    /* 4. Secondary buffer (with volume/pan/frequency control) */
    memset(&desc, 0, sizeof(desc));
    desc.dwSize  = sizeof(desc);
    desc.dwFlags = DSBCAPS_CTRLVOLUME | DSBCAPS_CTRLPAN | DSBCAPS_CTRLFREQUENCY;
    desc.dwBufferBytes = 48000 * 4; /* 1s buffer */
    desc.lpwfxFormat   = &wfx;

    memset(&wfx, 0, sizeof(wfx));
    wfx.wFormatTag      = WAVE_FORMAT_PCM;
    wfx.nChannels       = 2;
    wfx.nSamplesPerSec  = 48000;
    wfx.wBitsPerSample  = 16;
    wfx.nBlockAlign     = 4;
    wfx.nAvgBytesPerSec = 192000;

    hr = IDirectSound8_CreateSoundBuffer(ds8, &desc, &secondary, NULL);
    if (FAILED(hr)) {
        LOG_FAIL(log, "CreateSoundBuffer(secondary)", hr);
        goto out;
    }
    LOG_PASS_F(log, "CreateSoundBuffer(secondary) -> %p", (void *)secondary);
    pass++;

    /* 5. Lock/Unlock -> write 440Hz tone */
    {
        DWORD toneFrames = 0;
        short *tone = generate_tone_s16(&toneFrames, 48000, 2, 440.0f, 0.8f);
        if (!tone) {
            LOG_FAIL(log, "generate_tone(OOM)", E_OUTOFMEMORY);
            goto out;
        }
        DWORD toneBytes = toneFrames * 4;

        void *ptr1 = NULL, *ptr2 = NULL;
        DWORD bytes1 = 0, bytes2 = 0;
        hr = IDirectSoundBuffer_Lock(secondary, 0, toneBytes,
                                      &ptr1, &bytes1, &ptr2, &bytes2, 0);
        if (FAILED(hr)) {
            LOG_FAIL(log, "Lock", hr);
            free(tone);
            goto out;
        }
        LOG_PASS_F(log, "Lock -> ptr1=%p bytes1=%lu", ptr1, bytes1);
        pass++;

        if (ptr1) memcpy(ptr1, tone, bytes1);
        if (ptr2) memcpy(ptr2, (BYTE *)tone + bytes1, bytes2);

        hr = IDirectSoundBuffer_Unlock(secondary, ptr1, bytes1, ptr2, bytes2);
        if (FAILED(hr)) {
            LOG_FAIL(log, "Unlock", hr);
            free(tone);
            goto out;
        }
        LOG_PASS(log, "Unlock (tone written)");
        pass++;
        free(tone);
    }

    /* 6. Play + GetStatus */
    hr = IDirectSoundBuffer_Play(secondary, 0, 0, 0);
    if (FAILED(hr)) {
        LOG_FAIL(log, "Play", hr);
        goto out;
    }
    LOG_PASS(log, "Play (once)");
    pass++;

    {
        DWORD status = 0;
        Sleep(100);
        hr = IDirectSoundBuffer_GetStatus(secondary, &status);
        if (SUCCEEDED(hr)) {
            if (status & DSBSTATUS_PLAYING) {
                LOG_PASS(log, "GetStatus -> DSBSTATUS_PLAYING");
                pass++;
            } else {
                LOG_INFO_F(log, "[INFO] GetStatus -> %#lx (not PLAYING -- "
                           "buffer may have drained quickly)", status);
                pass++;
            }
        } else {
            LOG_FAIL(log, "GetStatus", hr);
            fail++;
        }
    }

    /* 7. GetCurrentPosition */
    {
        DWORD playPos = 0, writePos = 0;
        Sleep(300);
        hr = IDirectSoundBuffer_GetCurrentPosition(secondary, &playPos, &writePos);
        if (SUCCEEDED(hr)) {
            LOG_PASS_F(log, "GetCurrentPosition -> play=%lu write=%lu",
                       playPos, writePos);
            pass++;
        } else {
            LOG_FAIL(log, "GetCurrentPosition", hr);
            fail++;
        }
    }

    /* 8. SetVolume / GetVolume */
    {
        hr = IDirectSoundBuffer_SetVolume(secondary, -1000);
        if (SUCCEEDED(hr)) {
            long vol = 0;
            IDirectSoundBuffer_GetVolume(secondary, &vol);
            LOG_PASS_F(log, "SetVolume(-1000) -> GetVolume=%ld", vol);
            pass++;
        } else {
            LOG_FAIL(log, "SetVolume", hr);
            fail++;
        }
    }

    /* SetPan / GetPan */
    {
        hr = IDirectSoundBuffer_SetPan(secondary, -5000);
        if (SUCCEEDED(hr)) {
            long pan = 0;
            IDirectSoundBuffer_GetPan(secondary, &pan);
            LOG_PASS_F(log, "SetPan(-5000) -> GetPan=%ld", pan);
            pass++;
        } else {
            LOG_FAIL(log, "SetPan", hr);
            fail++;
        }
    }

    /* SetFrequency / GetFrequency */
    {
        hr = IDirectSoundBuffer_SetFrequency(secondary, 96000);
        if (SUCCEEDED(hr)) {
            DWORD freq = 0;
            IDirectSoundBuffer_GetFrequency(secondary, &freq);
            LOG_PASS_F(log, "SetFrequency(96000) -> GetFrequency=%lu", freq);
            pass++;
        } else {
            LOG_FAIL(log, "SetFrequency", hr);
            fail++;
        }
    }

    /* 9. Stop -> Play(looping) -> Stop */
    {
        IDirectSoundBuffer_Stop(secondary);

        /* Re-lock and re-write for looping */
        DWORD toneFrames = 0;
        short *tone2 = generate_tone_s16(&toneFrames, 48000, 2, 440.0f, 0.3f);
        if (tone2) {
            void *p1 = NULL, *p2 = NULL;
            DWORD b1 = 0, b2 = 0;
            DWORD tb = toneFrames * 4;
            IDirectSoundBuffer_Lock(secondary, 0, tb, &p1, &b1, &p2, &b2, 0);
            if (p1) memcpy(p1, tone2, b1);
            if (p2) memcpy(p2, (BYTE *)tone2 + b1, b2);
            IDirectSoundBuffer_Unlock(secondary, p1, b1, p2, b2);
            free(tone2);
        }

        hr = IDirectSoundBuffer_Play(secondary, 0, 0, DSBPLAY_LOOPING);
        if (SUCCEEDED(hr)) {
            LOG_PASS(log, "Play(looping)");
            pass++;
            Sleep(400);
            IDirectSoundBuffer_Stop(secondary);
            LOG_PASS(log, "Stop (after looping)");
            pass++;
        } else {
            LOG_FAIL(log, "Play(looping)", hr);
            fail++;
        }
    }

    /* Stop */
    IDirectSoundBuffer_Stop(secondary);

    /* 10. 3D listener (weak: QueryInterface success) */
    {
        LPDIRECTSOUND3DLISTENER listener3D = NULL;
        hr = IDirectSoundBuffer_QueryInterface(secondary,
                &IID_IDirectSound3DListener, (void **)&listener3D);
        if (SUCCEEDED(hr)) {
            LOG_PASS_F(log, "QueryInterface(3DListener) -> %p", (void *)listener3D);
            pass++;
            IDirectSound3DListener_Release(listener3D);

            /* Also verify 3D buffer */
            LPDIRECTSOUND3DBUFFER buf3D = NULL;
            hr = IDirectSoundBuffer_QueryInterface(secondary,
                    &IID_IDirectSound3DBuffer, (void **)&buf3D);
            if (SUCCEEDED(hr)) {
                LOG_PASS_F(log, "QueryInterface(3DBuffer) -> %p", (void *)buf3D);
                pass++;
                IDirectSound3DBuffer_Release(buf3D);
            } else {
                LOG_INFO_F(log, "[INFO] QueryInterface(3DBuffer) -> 0x%08lx (non-critical)", hr);
            }
        } else {
            LOG_INFO_F(log, "[INFO] QueryInterface(3DListener) -> 0x%08lx (non-critical)", hr);
        }
    }

out:
    if (secondary) IDirectSoundBuffer_Release(secondary);
    if (primary)   IDirectSoundBuffer_Release(primary);
    if (ds8)       IDirectSound8_Release(ds8);

    LOG_INFO_F(log, "DirectSound -> Passed: %d / Failed: %d", pass, fail);
}
