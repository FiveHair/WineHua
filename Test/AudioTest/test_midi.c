/*
 * WineHua Audio Test -- MIDI / winmm verification
 *
 * Tests midiOut enumeration, open/close callbacks, short messages,
 * SysEx long messages, volume, reset, and close.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <string.h>

#include "test_common.h"

static volatile LONG g_mom_open_count = 0;
static volatile LONG g_mom_close_count = 0;
static volatile LONG g_mom_done_count = 0;

static void CALLBACK midi_callback(HMIDIOUT hmo, UINT uMsg,
                                   DWORD_PTR dwInstance,
                                   DWORD_PTR dwParam1,
                                   DWORD_PTR dwParam2)
{
    (void)hmo;
    (void)dwInstance;
    (void)dwParam1;
    (void)dwParam2;

    if (uMsg == MOM_OPEN)
        InterlockedIncrement(&g_mom_open_count);
    else if (uMsg == MOM_CLOSE)
        InterlockedIncrement(&g_mom_close_count);
    else if (uMsg == MOM_DONE)
        InterlockedIncrement(&g_mom_done_count);
}

void run_midi_tests(LogFn log)
{
    static const BYTE gm_reset[] = { 0xf0, 0x7e, 0x7f, 0x09, 0x01, 0xf7 };
    HMIDIOUT hmo = NULL;
    MMRESULT mmr;
    UINT numDevs;
    int pass = 0, fail = 0;

    LOG_INFO(log, "=== MIDI / winmm Verification ===");

    numDevs = midiOutGetNumDevs();
    if (numDevs == 0) {
        LOG_FAIL(log, "midiOutGetNumDevs -> 0 (no devices)", HRESULT_FROM_WIN32(MMSYSERR_NODRIVER));
        return;
    }
    LOG_PASS_F(log, "midiOutGetNumDevs -> %u device(s)", numDevs);
    pass++;

    {
        MIDIOUTCAPSA caps;
        memset(&caps, 0, sizeof(caps));
        mmr = midiOutGetDevCapsA(0, &caps, sizeof(caps));
        if (mmr == MMSYSERR_NOERROR) {
            LOG_PASS_F(log, "midiOutGetDevCaps -> name='%s' tech=%u voices=%u notes=%u support=%#lx",
                       caps.szPname, caps.wTechnology, caps.wVoices, caps.wNotes, caps.dwSupport);
            pass++;
        } else {
            LOG_FAIL_F(log, "midiOutGetDevCaps -> 0x%08lx",
                       (unsigned long)HRESULT_FROM_WIN32(mmr));
            fail++;
        }
    }

    g_mom_open_count = 0;
    g_mom_close_count = 0;
    g_mom_done_count = 0;

    mmr = midiOutOpen(&hmo, 0, (DWORD_PTR)midi_callback, 0, CALLBACK_FUNCTION);
    if (mmr != MMSYSERR_NOERROR) {
        LOG_FAIL_F(log, "midiOutOpen -> 0x%08lx", (unsigned long)HRESULT_FROM_WIN32(mmr));
        return;
    }
    LOG_PASS_F(log, "midiOutOpen -> %p", (void *)hmo);
    pass++;

    Sleep(50);
    if (g_mom_open_count > 0) {
        LOG_PASS_F(log, "MOM_OPEN callback received (count=%ld)", (long)g_mom_open_count);
        pass++;
    } else {
        LOG_FAIL(log, "MOM_OPEN callback timeout", HRESULT_FROM_WIN32(ERROR_TIMEOUT));
        fail++;
    }

    mmr = midiOutShortMsg(hmo, 0x000005c0u);
    if (mmr == MMSYSERR_NOERROR) {
        LOG_PASS(log, "midiOutShortMsg(program change)");
        pass++;
    } else {
        LOG_FAIL_F(log, "midiOutShortMsg(program change) -> 0x%08lx",
                   (unsigned long)HRESULT_FROM_WIN32(mmr));
        fail++;
    }

    mmr = midiOutShortMsg(hmo, 0x00403c90u);
    if (mmr == MMSYSERR_NOERROR) {
        LOG_PASS(log, "midiOutShortMsg(note on C4)");
        pass++;
        Sleep(200);
        midiOutShortMsg(hmo, 0x00003c80u);
        Sleep(50);
    } else {
        LOG_FAIL_F(log, "midiOutShortMsg(note on) -> 0x%08lx",
                   (unsigned long)HRESULT_FROM_WIN32(mmr));
        fail++;
    }

    {
        DWORD originalVol = 0, testVol = 0x80008000, currentVol = 0;

        mmr = midiOutGetVolume(hmo, &originalVol);
        if (mmr == MMSYSERR_NOERROR) {
            LOG_PASS_F(log, "midiOutGetVolume -> %#lx", originalVol);
            pass++;
            mmr = midiOutSetVolume(hmo, testVol);
            if (mmr == MMSYSERR_NOERROR) {
                midiOutGetVolume(hmo, &currentVol);
                LOG_PASS_F(log, "midiOutSetVolume(%#lx) -> GetVolume=%#lx", testVol, currentVol);
                pass++;
                midiOutSetVolume(hmo, originalVol);
            } else {
                LOG_FAIL_F(log, "midiOutSetVolume -> 0x%08lx",
                           (unsigned long)HRESULT_FROM_WIN32(mmr));
                fail++;
            }
        } else {
            LOG_FAIL_F(log, "midiOutGetVolume -> 0x%08lx",
                       (unsigned long)HRESULT_FROM_WIN32(mmr));
            fail++;
        }
    }

    {
        MIDIHDR hdr;
        BYTE sysex[sizeof(gm_reset)];
        int waited = 0;

        memcpy(sysex, gm_reset, sizeof(gm_reset));
        memset(&hdr, 0, sizeof(hdr));
        hdr.lpData = (LPSTR)sysex;
        hdr.dwBufferLength = sizeof(sysex);

        mmr = midiOutPrepareHeader(hmo, &hdr, sizeof(hdr));
        if (mmr == MMSYSERR_NOERROR) {
            LOG_PASS(log, "midiOutPrepareHeader(SysEx)");
            pass++;
        } else {
            LOG_FAIL_F(log, "midiOutPrepareHeader -> 0x%08lx",
                       (unsigned long)HRESULT_FROM_WIN32(mmr));
            fail++;
            goto out;
        }

        g_mom_done_count = 0;
        mmr = midiOutLongMsg(hmo, &hdr, sizeof(hdr));
        if (mmr == MMSYSERR_NOERROR) {
            LOG_PASS(log, "midiOutLongMsg(GM reset SysEx)");
            pass++;
        } else {
            LOG_FAIL_F(log, "midiOutLongMsg -> 0x%08lx",
                       (unsigned long)HRESULT_FROM_WIN32(mmr));
            fail++;
        }

        while (g_mom_done_count == 0 && waited < 40) {
            Sleep(50);
            waited++;
        }
        if (g_mom_done_count > 0) {
            LOG_PASS_F(log, "MOM_DONE callback received (count=%ld)", (long)g_mom_done_count);
            pass++;
        } else {
            LOG_FAIL(log, "MOM_DONE callback timeout", HRESULT_FROM_WIN32(ERROR_TIMEOUT));
            fail++;
        }

        mmr = midiOutUnprepareHeader(hmo, &hdr, sizeof(hdr));
        if (mmr == MMSYSERR_NOERROR) {
            LOG_PASS(log, "midiOutUnprepareHeader(SysEx)");
            pass++;
        } else {
            LOG_FAIL_F(log, "midiOutUnprepareHeader -> 0x%08lx",
                       (unsigned long)HRESULT_FROM_WIN32(mmr));
            fail++;
        }
    }

    mmr = midiOutReset(hmo);
    if (mmr == MMSYSERR_NOERROR) {
        LOG_PASS(log, "midiOutReset");
        pass++;
    } else {
        LOG_FAIL_F(log, "midiOutReset -> 0x%08lx",
                   (unsigned long)HRESULT_FROM_WIN32(mmr));
        fail++;
    }

out:
    if (hmo) {
        mmr = midiOutClose(hmo);
        if (mmr == MMSYSERR_NOERROR) {
            Sleep(50);
            LOG_PASS(log, "midiOutClose");
            pass++;
            if (g_mom_close_count > 0) {
                LOG_PASS_F(log, "MOM_CLOSE callback received (count=%ld)", (long)g_mom_close_count);
                pass++;
            } else {
                LOG_FAIL(log, "MOM_CLOSE callback timeout", HRESULT_FROM_WIN32(ERROR_TIMEOUT));
                fail++;
            }
        } else {
            LOG_FAIL_F(log, "midiOutClose -> 0x%08lx",
                       (unsigned long)HRESULT_FROM_WIN32(mmr));
            fail++;
        }
    }

    LOG_INFO_F(log, "MIDI -> Passed: %d / Failed: %d", pass, fail);
}
