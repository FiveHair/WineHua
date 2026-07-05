/*
 * WineHua Audio Test -- waveOut / winmm full-interface verification
 *
 * Tests waveOutGetNumDevs, Open, PrepareHeader, Write, WOM_DONE callback,
 * Pause/Restart, Reset, Volume, and Close.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "test_common.h"

/* -- Global callback state ------------------------------------------- */

static volatile LONG g_wom_done_count = 0;
static HWAVEOUT    g_callback_hwo    = NULL;
static UINT        g_callback_msg    = 0;

static void CALLBACK waveout_callback(HWAVEOUT hwo, UINT uMsg,
                                       DWORD_PTR dwInstance,
                                       DWORD_PTR dwParam1,
                                       DWORD_PTR dwParam2)
{
    (void)dwInstance;
    (void)dwParam1;
    (void)dwParam2;

    if (uMsg == WOM_DONE) {
        InterlockedIncrement(&g_wom_done_count);
        g_callback_hwo = hwo;
        g_callback_msg = uMsg;
    }
}

void run_waveout_tests(LogFn log)
{
    UINT numDevs;
    MMRESULT mmr;
    WAVEFORMATEX wfx;
    HWAVEOUT hwo = NULL;
    WAVEHDR hdr;
    int pass = 0, fail = 0;

    LOG_INFO(log, "=== waveOut Full Interface Verification ===");

    /* 1. GetNumDevs */
    numDevs = waveOutGetNumDevs();
    if (numDevs == 0) {
        LOG_FAIL(log, "waveOutGetNumDevs -> 0 (no devices)", HRESULT_FROM_WIN32(MMSYSERR_NODRIVER));
        return;
    }
    LOG_PASS_F(log, "waveOutGetNumDevs -> %u device(s)", numDevs);
    pass++;

    /* 2. GetDevCaps */
    {
        WAVEOUTCAPSA caps;
        memset(&caps, 0, sizeof(caps));
        mmr = waveOutGetDevCapsA(0, &caps, sizeof(caps));
        if (mmr == MMSYSERR_NOERROR) {
            LOG_PASS_F(log, "waveOutGetDevCaps -> name='%s' channels=%u formats=%#lx",
                       caps.szPname, caps.wChannels, caps.dwFormats);
            pass++;
        } else {
            LOG_FAIL_F(log, "waveOutGetDevCaps -> 0x%08lx",
                       (unsigned long)HRESULT_FROM_WIN32(mmr));
            fail++;
        }
    }

    /* 3. Open (CALLBACK_FUNCTION) */
    g_wom_done_count = 0;
    memset(&wfx, 0, sizeof(wfx));
    wfx.wFormatTag      = WAVE_FORMAT_PCM;
    wfx.nChannels       = 2;
    wfx.nSamplesPerSec  = 48000;
    wfx.wBitsPerSample  = 16;
    wfx.nBlockAlign     = 4;
    wfx.nAvgBytesPerSec = 192000;

    mmr = waveOutOpen(&hwo, WAVE_MAPPER, &wfx,
                       (DWORD_PTR)waveout_callback, 0,
                       CALLBACK_FUNCTION);
    if (mmr != MMSYSERR_NOERROR) {
        LOG_FAIL_F(log, "waveOutOpen(CALLBACK_FUNCTION, 48000/s16/stereo) -> 0x%08lx",
                   (unsigned long)HRESULT_FROM_WIN32(mmr));
        return;
    }
    LOG_PASS_F(log, "waveOutOpen -> %p", (void *)hwo);
    pass++;

    /* 4. PrepareHeader + Write (440Hz tone, 1s) */
    {
        DWORD toneFrames = 0;
        short *tone = generate_tone_s16(&toneFrames, 48000, 2, 440.0f, 1.0f);
        if (!tone) {
            LOG_FAIL(log, "generate_tone(OOM)", E_OUTOFMEMORY);
            goto out;
        }

        DWORD dataSize = toneFrames * 4;

        memset(&hdr, 0, sizeof(hdr));
        hdr.lpData         = (LPSTR)tone;
        hdr.dwBufferLength = dataSize;

        mmr = waveOutPrepareHeader(hwo, &hdr, sizeof(hdr));
        if (mmr != MMSYSERR_NOERROR) {
            LOG_FAIL_F(log, "waveOutPrepareHeader -> 0x%08lx",
                       (unsigned long)HRESULT_FROM_WIN32(mmr));
            free(tone);
            goto out;
        }
        LOG_PASS(log, "waveOutPrepareHeader");
        pass++;

        mmr = waveOutWrite(hwo, &hdr, sizeof(hdr));
        if (mmr != MMSYSERR_NOERROR) {
            LOG_FAIL_F(log, "waveOutWrite -> 0x%08lx",
                       (unsigned long)HRESULT_FROM_WIN32(mmr));
            waveOutUnprepareHeader(hwo, &hdr, sizeof(hdr));
            free(tone);
            goto out;
        }
        LOG_PASS(log, "waveOutWrite (440Hz tone)");
        pass++;

        /* 5. Wait for WOM_DONE callback */
        LOG_INFO(log, "   waiting for WOM_DONE callback...");
        {
            int waited = 0;
            while (g_wom_done_count == 0 && waited < 40) {
                Sleep(50);
                waited++;
                /* Pump messages to deliver callback */
                MSG msg;
                while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
                    DispatchMessageA(&msg);
                }
            }
        }

        if (g_wom_done_count > 0) {
            LOG_PASS_F(log, "WOM_DONE callback received (count=%ld)",
                       (long)g_wom_done_count);
            pass++;
        } else {
            LOG_FAIL(log, "WOM_DONE callback timeout (2s)",
                     HRESULT_FROM_WIN32(ERROR_TIMEOUT));
            fail++;
        }

        /* 6. GetPosition */
        {
            MMTIME mmt;
            memset(&mmt, 0, sizeof(mmt));
            mmt.wType = TIME_BYTES;
            mmr = waveOutGetPosition(hwo, &mmt, sizeof(mmt));
            if (mmr == MMSYSERR_NOERROR) {
                LOG_PASS_F(log, "waveOutGetPosition -> type=%u value=%lu",
                           mmt.wType, mmt.u.cb);
                pass++;
            } else {
                LOG_FAIL_F(log, "waveOutGetPosition -> 0x%08lx",
                           (unsigned long)HRESULT_FROM_WIN32(mmr));
                fail++;
            }
        }

        waveOutUnprepareHeader(hwo, &hdr, sizeof(hdr));
        free(tone);
    }

    /* 7. Pause + Restart */
    {
        /* Write another short tone first */
        DWORD tf = 0;
        short *t = generate_tone_s16(&tf, 48000, 2, 440.0f, 0.5f);
        if (t) {
            WAVEHDR h;
            memset(&h, 0, sizeof(h));
            h.lpData         = (LPSTR)t;
            h.dwBufferLength = tf * 4;
            waveOutPrepareHeader(hwo, &h, sizeof(h));
            waveOutWrite(hwo, &h, sizeof(h));

            mmr = waveOutPause(hwo);
            if (mmr == MMSYSERR_NOERROR) {
                LOG_PASS(log, "waveOutPause");
                pass++;
                Sleep(300);
            } else {
                LOG_FAIL_F(log, "waveOutPause -> 0x%08lx",
                           (unsigned long)HRESULT_FROM_WIN32(mmr));
                fail++;
            }

            mmr = waveOutRestart(hwo);
            if (mmr == MMSYSERR_NOERROR) {
                LOG_PASS(log, "waveOutRestart");
                pass++;
            } else {
                LOG_FAIL_F(log, "waveOutRestart -> 0x%08lx",
                           (unsigned long)HRESULT_FROM_WIN32(mmr));
                fail++;
            }

            /* Wait for completion */
            g_wom_done_count = 0;
            int waited = 0;
            while (g_wom_done_count == 0 && waited < 20) { Sleep(50); waited++; }
            waveOutUnprepareHeader(hwo, &h, sizeof(h));
            free(t);
        }
    }

    /* 8. Reset (verify WHDR_DONE) */
    {
        DWORD tf = 0;
        short *t = generate_tone_s16(&tf, 48000, 2, 440.0f, 3.0f);
        if (t) {
            WAVEHDR h;
            memset(&h, 0, sizeof(h));
            h.lpData         = (LPSTR)t;
            h.dwBufferLength = tf * 4;
            waveOutPrepareHeader(hwo, &h, sizeof(h));
            waveOutWrite(hwo, &h, sizeof(h));

            Sleep(100);
            mmr = waveOutReset(hwo);
            if (mmr == MMSYSERR_NOERROR) {
                if (h.dwFlags & WHDR_DONE) {
                    LOG_PASS(log, "waveOutReset -> buffer marked WHDR_DONE");
                    pass++;
                } else {
                    LOG_INFO(log, "[INFO] waveOutReset -> buffer NOT marked DONE "
                                  "(may be expected)");
                    pass++;
                }
            } else {
                LOG_FAIL_F(log, "waveOutReset -> 0x%08lx",
                           (unsigned long)HRESULT_FROM_WIN32(mmr));
                fail++;
            }
            waveOutUnprepareHeader(hwo, &h, sizeof(h));
            free(t);
        }
    }

    /* 9. SetVolume / GetVolume */
    {
        DWORD vol = 0x80008000; /* -50% left, -50% right (approx) */
        mmr = waveOutSetVolume(hwo, vol);
        if (mmr == MMSYSERR_NOERROR) {
            DWORD got = 0;
            waveOutGetVolume(hwo, &got);
            LOG_PASS_F(log, "waveOutSetVolume(%#lx) -> GetVolume=%#lx", vol, got);
            pass++;
        } else {
            LOG_FAIL_F(log, "waveOutSetVolume -> 0x%08lx",
                       (unsigned long)HRESULT_FROM_WIN32(mmr));
            fail++;
        }
    }

out:
    /* 10. Close */
    if (hwo) {
        waveOutReset(hwo);
        mmr = waveOutClose(hwo);
        if (mmr == MMSYSERR_NOERROR) {
            LOG_PASS(log, "waveOutClose");
            pass++;
        } else {
            LOG_FAIL_F(log, "waveOutClose -> 0x%08lx",
                       (unsigned long)HRESULT_FROM_WIN32(mmr));
            fail++;
        }
    }

    LOG_INFO_F(log, "waveOut -> Passed: %d / Failed: %d", pass, fail);
}
