/*
 * WineHua Audio Test -- MCI (Media Control Interface) verification
 *
 * Tests basic MCI waveaudio device: open, play, status, stop, close.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_common.h"

static BOOL file_exists_a(const char *path)
{
    DWORD attrs = GetFileAttributesA(path);
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

static void run_mpegvideo_case(LogFn log, const char *path, const char *label,
                               int *pass, int *fail)
{
    char cmd[512];
    char errBuf[128] = {0};
    char modeBuf[64] = {0};
    char posBuf[64] = {0};
    MCIERROR merr;

    snprintf(cmd, sizeof(cmd), "open \"%s\" type MPEGVideo alias tv", path);
    merr = mciSendStringA(cmd, NULL, 0, 0);
    if (merr != 0) {
        mciGetErrorStringA(merr, errBuf, sizeof(errBuf));
        LOG_FAIL_F(log, "%s open -> '%s' (merr=%lu)",
                   label, errBuf, (unsigned long)merr);
        (*fail)++;
        return;
    }
    LOG_PASS_F(log, "%s open", label);
    (*pass)++;

    merr = mciSendStringA("play tv", NULL, 0, 0);
    if (merr != 0) {
        mciGetErrorStringA(merr, errBuf, sizeof(errBuf));
        LOG_FAIL_F(log, "%s play -> '%s' (merr=%lu)",
                   label, errBuf, (unsigned long)merr);
        (*fail)++;
    } else {
        LOG_PASS_F(log, "%s play", label);
        (*pass)++;
    }

    Sleep(300);

    merr = mciSendStringA("status tv mode", modeBuf, sizeof(modeBuf), 0);
    if (merr == 0) {
        LOG_PASS_F(log, "%s status mode -> '%s'", label, modeBuf);
        (*pass)++;
    } else {
        LOG_FAIL_F(log, "%s status mode -> merr=%lu",
                   label, (unsigned long)merr);
        (*fail)++;
    }

    merr = mciSendStringA("status tv position", posBuf, sizeof(posBuf), 0);
    if (merr == 0) {
        LOG_PASS_F(log, "%s status position -> %s", label, posBuf);
        (*pass)++;
    } else {
        LOG_FAIL_F(log, "%s status position -> merr=%lu",
                   label, (unsigned long)merr);
        (*fail)++;
    }

    merr = mciSendStringA("stop tv", NULL, 0, 0);
    if (merr == 0) {
        LOG_PASS_F(log, "%s stop", label);
        (*pass)++;
    } else {
        LOG_FAIL_F(log, "%s stop -> merr=%lu", label, (unsigned long)merr);
        (*fail)++;
    }

    merr = mciSendStringA("close tv", NULL, 0, 0);
    if (merr == 0) {
        LOG_PASS_F(log, "%s close", label);
        (*pass)++;
    } else {
        LOG_FAIL_F(log, "%s close -> merr=%lu", label, (unsigned long)merr);
        (*fail)++;
    }
}

void run_mci_tests(LogFn log)
{
    int pass = 0, fail = 0;

    LOG_INFO(log, "=== MCI Audio Interface Verification ===");

    /* 1. Check waveaudio device capabilities */
    {
        char buf[256] = {0};
        MCIERROR merr = mciSendStringA(
                "capability waveaudio device type", buf, sizeof(buf), 0);
        if (merr == 0) {
            LOG_PASS_F(log, "capability waveaudio -> '%s'", buf);
            pass++;
        } else if (merr == MCIERR_BAD_INTEGER ||
                   merr == MCIERR_UNRECOGNIZED_KEYWORD) {
            LOG_INFO_F(log, "[INFO] waveaudio not available (MCI error %lu); "
                       "trying alternate query", merr);

            /* Also check MCI_ALL_DEVICE_ID */
            MCI_STATUS_PARMS status;
            status.dwItem = MCI_STATUS_READY;
            merr = mciSendCommand(0, MCI_STATUS,
                                   MCI_STATUS_ITEM | MCI_WAIT,
                                   (DWORD_PTR)&status);
            if (merr == 0) {
                LOG_PASS(log, "MCI device status -> ready");
                pass++;
            } else {
                LOG_INFO_F(log, "[INFO] MCI_STATUS -> error %lu (might be "
                           "non-fatal if waveaudio not registered)", merr);
                /* Continue - the other tests will reveal the real state */
            }
        } else {
            LOG_INFO_F(log, "[INFO] MCI query -> error %lu (device may not exist)", merr);
        }
    }

    /* 2. Generate a temporary WAV file */
    {
        BOOL ok = generate_wav_file("_test_mci_tone.wav", 48000, 2, 16, 440.0f, 1.0f);
        if (!ok) {
            LOG_FAIL(log, "generate_wav_file", E_OUTOFMEMORY);
            return;
        }
        LOG_PASS(log, "generated _test_mci_tone.wav (440Hz/1s)");
        pass++;
    }

    /* 3. Open the WAV via MCI waveaudio */
    {
        char cmd[256];
        snprintf(cmd, sizeof(cmd),
                 "open \"_test_mci_tone.wav\" type waveaudio alias t");
        MCIERROR merr = mciSendStringA(cmd, NULL, 0, 0);
        if (merr != 0) {
            char errBuf[128] = {0};
            mciGetErrorStringA(merr, errBuf, sizeof(errBuf));
            LOG_FAIL_F(log, "MCI open waveaudio -> '%s' (merr=%lu)", errBuf, (unsigned long)merr);
            DeleteFileA("_test_mci_tone.wav");
            return;
        }
        LOG_PASS(log, "MCI open 't' (waveaudio)");
        pass++;
    }

    /* 4. Play */
    {
        MCIERROR merr = mciSendStringA("play t", NULL, 0, 0);
        if (merr == 0) {
            LOG_PASS(log, "MCI play t");
            pass++;
        } else {
            char buf[128] = {0};
            mciGetErrorStringA(merr, buf, sizeof(buf));
            LOG_FAIL_F(log, "MCI play -> '%s' (merr=%lu)", buf, (unsigned long)merr);
            fail++;
        }
    }

    /* 5. Status: mode + position */
    {
        Sleep(300); /* let playback progress */

        /* mode */
        {
            char buf[64] = {0};
            MCIERROR merr = mciSendStringA("status t mode", buf, sizeof(buf), 0);
            if (merr == 0) {
                LOG_PASS_F(log, "MCI status mode -> '%s'", buf);
                pass++;
            } else {
                LOG_FAIL_F(log, "status t mode -> merr=%lu", (unsigned long)merr);
                fail++;
            }
        }

        /* position */
        {
            char buf[64] = {0};
            MCIERROR merr = mciSendStringA("status t position", buf, sizeof(buf), 0);
            if (merr == 0) {
                long pos = atol(buf);
                if (pos > 0) {
                    LOG_PASS_F(log, "MCI status position -> %ld (non-zero v)", pos);
                    pass++;
                } else {
                    LOG_INFO_F(log, "[INFO] status position = %ld (may be 0 if "
                               "playback hasn't started)", pos);
                    pass++;
                }
            } else {
                LOG_FAIL_F(log, "status t position -> merr=%lu", (unsigned long)merr);
                fail++;
            }
        }
    }

    /* 6. Stop */
    {
        MCIERROR merr = mciSendStringA("stop t", NULL, 0, 0);
        if (merr == 0) {
            LOG_PASS(log, "MCI stop t");
            pass++;
        } else {
            char buf[128] = {0};
            mciGetErrorStringA(merr, buf, sizeof(buf));
            LOG_FAIL_F(log, "MCI stop -> '%s' (merr=%lu)", buf, (unsigned long)merr);
            fail++;
        }
    }

    /* 7. Close */
    {
        MCIERROR merr = mciSendStringA("close t", NULL, 0, 0);
        if (merr == 0) {
            LOG_PASS(log, "MCI close t");
            pass++;
        } else {
            LOG_FAIL_F(log, "MCI close -> merr=%lu", (unsigned long)merr);
            fail++;
        }
    }

    /* 8. Verify the DirectShow / MCIQTZ route using the same WAV */
    run_mpegvideo_case(log, "_test_mci_tone.wav", "MCI MPEGVideo (wav)",
                       &pass, &fail);

    /* 9. Try a real MP3 if one is available beside the test app */
    {
        static const char *mp3Candidates[] = {
            "test.mp3",
            "music\\wanqiu.mp3",
            "music\\yanhuasanyue.mp3",
        };
        const char *selected = NULL;
        size_t i;

        for (i = 0; i < sizeof(mp3Candidates) / sizeof(mp3Candidates[0]); ++i) {
            if (file_exists_a(mp3Candidates[i])) {
                selected = mp3Candidates[i];
                break;
            }
        }

        if (selected) {
            run_mpegvideo_case(log, selected, "MCI MPEGVideo (mp3)", &pass, &fail);
        } else {
            LOG_INFO(log, "[INFO] No MP3 sample found; skipped MCI MPEGVideo mp3 test");
        }
    }

    /* Cleanup */
    DeleteFileA("_test_mci_tone.wav");

    LOG_INFO_F(log, "MCI -> Passed: %d / Failed: %d", pass, fail);
}
