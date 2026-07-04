/*
 * WineHua Audio Test v3 - WASAPI audio player
 * Build: x86_64-w64-mingw32-gcc -O2 -o winehua_audio_test.exe main_v3.c -lcomctl32 -lole32 -mwindows -static
 */
#define WIN32_LEAN_AND_MEAN
#define _WIN32_IE 0x0500
#define COBJMACROS
#include <windows.h>
#include <commctrl.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <math.h>
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_FLOAT_OUTPUT
#include "minimp3.h"
