
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mmx3_common.h"

extern "C" {
    typedef struct stb_vorbis stb_vorbis;
    typedef struct
    {
        char *alloc_buffer;
        int alloc_buffer_length_in_bytes;
    } stb_vorbis_alloc;
    typedef struct
    {
        unsigned int sample_rate;
        int channels;
        unsigned int setup_memory_required;
        unsigned int setup_temp_memory_required;
        unsigned int temp_memory_required;
        int max_frame_size;
    } stb_vorbis_info;

    stb_vorbis *stb_vorbis_open_filename(const char *filename, int *error, const stb_vorbis_alloc *alloc_buffer);
    stb_vorbis_info stb_vorbis_get_info(stb_vorbis *f);
    int stb_vorbis_get_samples_short_interleaved(stb_vorbis *f, int channels, short *buffer, int num_shorts);
    int stb_vorbis_seek_start(stb_vorbis *f);
    void stb_vorbis_close(stb_vorbis *f);
}

typedef HMMIO (WINAPI *MmioOpenAFn)(LPSTR, LPMMIOINFO, DWORD);
typedef MMRESULT (WINAPI *MmioCloseFn)(HMMIO, UINT);

static MmioOpenAFn g_realMmioOpenA = NULL;
static MmioCloseFn g_realMmioClose = NULL;

static BOOL g_externalOggEnabled = TRUE;
static BOOL g_externalOggLoop = TRUE;
static DWORD g_externalOggBufferCount = 3;
static DWORD g_externalOggBufferMs = 15;
static DWORD g_externalOggFadeOutMs = 250;
static char g_externalOggPath[MAX_PATH] = "BGM_EXT";

static CRITICAL_SECTION g_oggLock;
static BOOL g_oggLockInit = FALSE;
static HMMIO g_trackedHmmio = NULL;

static const BYTE g_silentWav[] = {
    'R','I','F','F', 0x34,0x00,0x00,0x00, 'W','A','V','E',
    'f','m','t',' ', 0x10,0x00,0x00,0x00, 0x01,0x00, 0x02,0x00,
    0x22,0x56,0x00,0x00, 0x88,0x58,0x01,0x00, 0x04,0x00, 0x10,0x00,
    'd','a','t','a', 0x10,0x00,0x00,0x00,
    0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0
};

struct OggBuffer
{
    WAVEHDR hdr;
    short *pcm;
};

struct OggPlayback
{
    HANDLE thread;
    HANDLE eventDone;
    volatile LONG stopRequested;
    volatile LONG fadeRequested;
    BOOL loop;
    DWORD fadeOutMs;
    DWORD bufferCount;
    DWORD bufferMs;
    char path[MAX_PATH];
    HWAVEOUT waveOut;
};

static OggPlayback *g_playback = NULL;

static void EnsureLock()
{
    if (!g_oggLockInit) {
        InitializeCriticalSection(&g_oggLock);
        g_oggLockInit = TRUE;
    }
}

static BOOL FileExistsA2(const char *path)
{
    DWORD attr = GetFileAttributesA(path);
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

static BOOL EndsWithNoCase(const char *text, const char *suffix)
{
    if (!text || !suffix) return FALSE;
    size_t lt = strlen(text);
    size_t ls = strlen(suffix);
    if (lt < ls) return FALSE;
    return _stricmp(text + lt - ls, suffix) == 0;
}

static BOOL ContainsNoCase(const char *text, const char *needle)
{
    if (!text || !needle || !needle[0]) return FALSE;
    size_t n = strlen(needle);
    for (const char *p = text; *p; ++p) {
        if (_strnicmp(p, needle, n) == 0) return TRUE;
    }
    return FALSE;
}

static const char *BaseName(const char *path)
{
    const char *a = strrchr(path, '\\');
    const char *b = strrchr(path, '/');
    const char *p = a > b ? a : b;
    return p ? p + 1 : path;
}

static void RemoveExt(char *s)
{
    char *dot = strrchr(s, '.');
    if (dot) *dot = 0;
}

static DWORD GetPortableConfigDword(const char *section, const char *key, DWORD defaultValue)
{
    char buf[64];
    wsprintfA(buf, "%lu", (unsigned long)defaultValue);
    GetPrivateProfileStringA(section, key, buf, buf, sizeof(buf), g_configPath);
    return (DWORD)strtoul(buf, NULL, 10);
}


static BOOL AudioIniKeyExists(const char *key)
{
    char value[128];
    const char *missing = "__MMX3_AUDIO_KEY_MISSING__";
    GetPrivateProfileStringA("Audio", key, missing, value, sizeof(value), g_configPath);
    return strcmp(value, missing) != 0;
}

static void EnsureAudioDefaultString(const char *key, const char *value)
{
    if (!AudioIniKeyExists(key)) {
        WritePrivateProfileStringA("Audio", key, value, g_configPath);
        LogLine("ExternalOGG: wrote default Audio.%s=%s", key, value);
    }
}

static void EnsureExternalOggAudioDefaults(void)
{
    EnsureAudioDefaultString("ExternalOggBgm", "True");
    EnsureAudioDefaultString("ExternalBgmPath", "BGM_EXT");
    EnsureAudioDefaultString("ExternalOggLoop", "True");
    EnsureAudioDefaultString("ExternalOggBufferCount", "3");
    EnsureAudioDefaultString("ExternalOggBufferMs", "60");
    EnsureAudioDefaultString("ExternalOggFadeOutMs", "500");
}

static void ClampConfig()
{
    if (g_externalOggBufferCount < 2) g_externalOggBufferCount = 2;
    if (g_externalOggBufferCount > 8) g_externalOggBufferCount = 8;
    if (g_externalOggBufferMs < 30) g_externalOggBufferMs = 30;
    if (g_externalOggBufferMs > 250) g_externalOggBufferMs = 250;
    if (g_externalOggFadeOutMs > 5000) g_externalOggFadeOutMs = 5000;
}

static BOOL BuildOggPath(const char *original, char *out, DWORD outSize)
{
    if (!original || !out || outSize == 0) return FALSE;
    if (!EndsWithNoCase(original, ".SE") && !EndsWithNoCase(original, ".WAV")) return FALSE;
    if (!ContainsNoCase(original, "\\BGM\\") && !ContainsNoCase(original, "/BGM/")) return FALSE;

    char name[MAX_PATH];
    lstrcpynA(name, BaseName(original), sizeof(name));
    RemoveExt(name);

    wsprintfA(out, "%s%s\\%s.ogg", g_gameDir, g_externalOggPath, name);
    return FileExistsA2(out);
}

static HMMIO OpenMemorySilentWav()
{
    if (!g_realMmioOpenA) return NULL;
    MMIOINFO info;
    ZeroMemory(&info, sizeof(info));
    info.fccIOProc = FOURCC_MEM;
    info.pchBuffer = (HPSTR)g_silentWav;
    info.cchBuffer = sizeof(g_silentWav);
    return g_realMmioOpenA(NULL, &info, MMIO_READ | MMIO_DENYWRITE);
}

static void FreePlayback(OggPlayback *pb)
{
    if (!pb) return;
    if (pb->eventDone) CloseHandle(pb->eventDone);
    free(pb);
}

static void HardStopPlaybackLocked()
{
    OggPlayback *pb = g_playback;
    if (!pb) return;
    g_playback = NULL;

    InterlockedExchange(&pb->stopRequested, 1);
    if (pb->waveOut) {
        waveOutReset(pb->waveOut);
    }
    if (pb->eventDone) SetEvent(pb->eventDone);

    HANDLE thread = pb->thread;
    LeaveCriticalSection(&g_oggLock);
    if (thread) WaitForSingleObject(thread, 1500);
    EnterCriticalSection(&g_oggLock);
    if (thread) CloseHandle(thread);
    FreePlayback(pb);
}

static void StopExternalOggPlaybackHard()
{
    EnsureLock();
    EnterCriticalSection(&g_oggLock);
    HardStopPlaybackLocked();
    LeaveCriticalSection(&g_oggLock);
}

static void RequestExternalOggFadeOut()
{
    EnsureLock();
    EnterCriticalSection(&g_oggLock);
    OggPlayback *pb = g_playback;
    if (!pb) {
        LeaveCriticalSection(&g_oggLock);
        return;
    }

    if (pb->fadeOutMs == 0) {
        LogLine("ExternalOGG: fadeOutMs=0 -> hard stop");
        HardStopPlaybackLocked();
        LeaveCriticalSection(&g_oggLock);
        return;
    }

    if (InterlockedCompareExchange(&pb->fadeRequested, 1, 0) == 0) {
        LogLine("ExternalOGG: fadeout requested ms=%lu", (unsigned long)pb->fadeOutMs);
    }
    LeaveCriticalSection(&g_oggLock);
}

static int FillPcmBuffer(stb_vorbis *vorbis, stb_vorbis_info info, OggPlayback *pb,
                         short *dst, int framesWanted, BOOL *fadeFinished,
                         int *fadeSamplesRemaining, int *fadeSamplesTotal)
{
    int channels = info.channels;
    int framesFilled = 0;
    *fadeFinished = FALSE;

    while (framesFilled < framesWanted) {
        if (InterlockedCompareExchange(&pb->stopRequested, 0, 0)) {
            break;
        }

        int requestFrames = framesWanted - framesFilled;
        int got = stb_vorbis_get_samples_short_interleaved(
            vorbis, channels, dst + framesFilled * channels, requestFrames * channels);

        if (got <= 0) {
            if (pb->loop && !InterlockedCompareExchange(&pb->fadeRequested, 0, 0)) {
                stb_vorbis_seek_start(vorbis);
                continue;
            }
            break;
        }

        if (InterlockedCompareExchange(&pb->fadeRequested, 0, 0)) {
            if (*fadeSamplesTotal <= 0) {
                *fadeSamplesTotal = (int)((info.sample_rate * pb->fadeOutMs) / 1000);
                if (*fadeSamplesTotal <= 0) *fadeSamplesTotal = 1;
                *fadeSamplesRemaining = *fadeSamplesTotal;
                LogLine("ExternalOGG: fadeout begin samples=%d", *fadeSamplesTotal);
            }

            for (int f = 0; f < got; ++f) {
                float gain = 0.0f;
                if (*fadeSamplesRemaining > 0) {
                    gain = (float)(*fadeSamplesRemaining) / (float)(*fadeSamplesTotal);
                    --(*fadeSamplesRemaining);
                }
                for (int c = 0; c < channels; ++c) {
                    int index = (framesFilled + f) * channels + c;
                    int v = (int)((float)dst[index] * gain);
                    if (v > 32767) v = 32767;
                    if (v < -32768) v = -32768;
                    dst[index] = (short)v;
                }
            }

            if (*fadeSamplesRemaining <= 0) {
                framesFilled += got;
                *fadeFinished = TRUE;
                break;
            }
        }

        framesFilled += got;
    }

    int totalSamples = framesWanted * channels;
    int filledSamples = framesFilled * channels;
    if (filledSamples < totalSamples) {
        ZeroMemory(dst + filledSamples, (totalSamples - filledSamples) * sizeof(short));
    }
    return framesFilled;
}

static DWORD WINAPI OggPlaybackThread(void *param)
{
    OggPlayback *pb = (OggPlayback *)param;
    int error = 0;
    stb_vorbis *vorbis = stb_vorbis_open_filename(pb->path, &error, NULL);
    if (!vorbis) {
        LogLine("ExternalOGG: stb_vorbis_open_filename failed path=\"%s\" error=%d", pb->path, error);
        return 0;
    }

    stb_vorbis_info info = stb_vorbis_get_info(vorbis);
    if (info.channels < 1 || info.channels > 2 || info.sample_rate == 0) {
        LogLine("ExternalOGG: unsupported format path=\"%s\" rate=%u channels=%d", pb->path, info.sample_rate, info.channels);
        stb_vorbis_close(vorbis);
        return 0;
    }

    WAVEFORMATEX fmt;
    ZeroMemory(&fmt, sizeof(fmt));
    fmt.wFormatTag = WAVE_FORMAT_PCM;
    fmt.nChannels = (WORD)info.channels;
    fmt.nSamplesPerSec = info.sample_rate;
    fmt.wBitsPerSample = 16;
    fmt.nBlockAlign = (WORD)(fmt.nChannels * (fmt.wBitsPerSample / 8));
    fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;

    MMRESULT mm = waveOutOpen(&pb->waveOut, WAVE_MAPPER, &fmt, (DWORD_PTR)pb->eventDone, 0, CALLBACK_EVENT);
    if (mm != MMSYSERR_NOERROR) {
        LogLine("ExternalOGG: waveOutOpen failed mm=%u", (unsigned)mm);
        stb_vorbis_close(vorbis);
        return 0;
    }

    int framesPerBuffer = (int)((info.sample_rate * pb->bufferMs) / 1000);
    if (framesPerBuffer < 256) framesPerBuffer = 256;
    DWORD bytesPerBuffer = framesPerBuffer * info.channels * sizeof(short);
    OggBuffer *buffers = (OggBuffer *)calloc(pb->bufferCount, sizeof(OggBuffer));
    if (!buffers) {
        waveOutClose(pb->waveOut);
        pb->waveOut = NULL;
        stb_vorbis_close(vorbis);
        return 0;
    }

    LogLine("ExternalOGG: streaming thread start path=\"%s\" rate=%u channels=%d buffers=%lu x %lums fade=%lums loop=%s",
            pb->path, info.sample_rate, info.channels,
            (unsigned long)pb->bufferCount, (unsigned long)pb->bufferMs,
            (unsigned long)pb->fadeOutMs, MMX3BoolText(pb->loop));

    int fadeSamplesRemaining = 0;
    int fadeSamplesTotal = 0;
    BOOL fadeFinished = FALSE;
    BOOL started = FALSE;

    for (DWORD i = 0; i < pb->bufferCount; ++i) {
        buffers[i].pcm = (short *)calloc(1, bytesPerBuffer);
        if (!buffers[i].pcm) continue;
        buffers[i].hdr.lpData = (LPSTR)buffers[i].pcm;
        buffers[i].hdr.dwBufferLength = bytesPerBuffer;
        waveOutPrepareHeader(pb->waveOut, &buffers[i].hdr, sizeof(WAVEHDR));
        FillPcmBuffer(vorbis, info, pb, buffers[i].pcm, framesPerBuffer,
                      &fadeFinished, &fadeSamplesRemaining, &fadeSamplesTotal);
        waveOutWrite(pb->waveOut, &buffers[i].hdr, sizeof(WAVEHDR));
        started = TRUE;
        if (fadeFinished) break;
    }

    while (started && !InterlockedCompareExchange(&pb->stopRequested, 0, 0)) {
        WaitForSingleObject(pb->eventDone, 20);
        BOOL anyQueued = FALSE;

        for (DWORD i = 0; i < pb->bufferCount; ++i) {
            WAVEHDR *hdr = &buffers[i].hdr;
            if (!buffers[i].pcm) continue;
            if (hdr->dwFlags & WHDR_DONE) {
                if (fadeFinished) {
                    continue;
                }
                hdr->dwFlags &= ~WHDR_DONE;
                FillPcmBuffer(vorbis, info, pb, buffers[i].pcm, framesPerBuffer,
                              &fadeFinished, &fadeSamplesRemaining, &fadeSamplesTotal);
                waveOutWrite(pb->waveOut, hdr, sizeof(WAVEHDR));
            }
            if (!(hdr->dwFlags & WHDR_DONE)) anyQueued = TRUE;
        }

        if (fadeFinished) {
            BOOL allDone = TRUE;
            for (DWORD i = 0; i < pb->bufferCount; ++i) {
                if (buffers[i].pcm && !(buffers[i].hdr.dwFlags & WHDR_DONE)) {
                    allDone = FALSE;
                    break;
                }
            }
            if (allDone) break;
        } else if (!anyQueued) {
            break;
        }
    }

    waveOutReset(pb->waveOut);
    for (DWORD i = 0; i < pb->bufferCount; ++i) {
        if (buffers[i].pcm) {
            waveOutUnprepareHeader(pb->waveOut, &buffers[i].hdr, sizeof(WAVEHDR));
            free(buffers[i].pcm);
        }
    }
    free(buffers);
    waveOutClose(pb->waveOut);
    pb->waveOut = NULL;
    stb_vorbis_close(vorbis);

    LogLine("ExternalOGG: streaming thread end path=\"%s\"", pb->path);
    return 0;
}

static BOOL StartExternalOggPlayback(const char *path)
{
    StopExternalOggPlaybackHard();

    OggPlayback *pb = (OggPlayback *)calloc(1, sizeof(OggPlayback));
    if (!pb) return FALSE;
    lstrcpynA(pb->path, path, sizeof(pb->path));
    pb->loop = g_externalOggLoop;
    pb->bufferCount = g_externalOggBufferCount;
    pb->bufferMs = g_externalOggBufferMs;
    pb->fadeOutMs = g_externalOggFadeOutMs;
    pb->eventDone = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (!pb->eventDone) {
        FreePlayback(pb);
        return FALSE;
    }

    EnsureLock();
    EnterCriticalSection(&g_oggLock);
    g_playback = pb;
    LeaveCriticalSection(&g_oggLock);

    pb->thread = CreateThread(NULL, 0, OggPlaybackThread, pb, 0, NULL);
    if (!pb->thread) {
        EnsureLock();
        EnterCriticalSection(&g_oggLock);
        if (g_playback == pb) g_playback = NULL;
        LeaveCriticalSection(&g_oggLock);
        FreePlayback(pb);
        return FALSE;
    }
    return TRUE;
}

static HMMIO WINAPI HookMmioOpenA(LPSTR filename, LPMMIOINFO info, DWORD flags)
{
    if (g_externalOggEnabled && filename) {
        char oggPath[MAX_PATH];
        if (BuildOggPath(filename, oggPath, sizeof(oggPath))) {
            LogLine("ExternalOGG: intercept original=\"%s\" ogg=\"%s\" flags=0x%08lX", filename, oggPath, (unsigned long)flags);
            if (StartExternalOggPlayback(oggPath)) {
                HMMIO hmmio = OpenMemorySilentWav();
                if (hmmio) {
                    EnsureLock();
                    EnterCriticalSection(&g_oggLock);
                    g_trackedHmmio = hmmio;
                    LeaveCriticalSection(&g_oggLock);
                    LogLine("ExternalOGG: redirected original audio to in-memory silent WAV hmmio=%p bytes=%lu", hmmio, (unsigned long)sizeof(g_silentWav));
                    LogLine("ExternalOGG: tracking hmmio=%p for lifecycle close", hmmio);
                    return hmmio;
                }
                LogLine("ExternalOGG: in-memory silent WAV open failed; hard stop and fallback original");
                StopExternalOggPlaybackHard();
            } else {
                LogLine("ExternalOGG: StartExternalOggPlayback failed; fallback original");
            }
        }
    }
    return g_realMmioOpenA ? g_realMmioOpenA(filename, info, flags) : NULL;
}

static MMRESULT WINAPI HookMmioClose(HMMIO hmmio, UINT flags)
{
    BOOL tracked = FALSE;
    EnsureLock();
    EnterCriticalSection(&g_oggLock);
    if (hmmio && hmmio == g_trackedHmmio) {
        g_trackedHmmio = NULL;
        tracked = TRUE;
    }
    LeaveCriticalSection(&g_oggLock);

    if (tracked) {
        LogLine("ExternalOGG: mmioClose tracked hmmio=%p -> fadeout", hmmio);
        RequestExternalOggFadeOut();
    }

    return g_realMmioClose ? g_realMmioClose(hmmio, flags) : MMSYSERR_ERROR;
}

void InstallExternalOggBgmHooks(HMODULE exe)
{
    EnsureLock();

    EnsureExternalOggAudioDefaults();

    g_externalOggEnabled = GetPortableConfigBool("Audio", "ExternalOggBgm", TRUE);
    g_externalOggLoop = GetPortableConfigBool("Audio", "ExternalOggLoop", TRUE);
    g_externalOggBufferCount = GetPortableConfigDword("Audio", "ExternalOggBufferCount", 3);
    g_externalOggBufferMs = GetPortableConfigDword("Audio", "ExternalOggBufferMs", 60);
    g_externalOggFadeOutMs = GetPortableConfigDword("Audio", "ExternalOggFadeOutMs", 500);
    GetPrivateProfileStringA("Audio", "ExternalBgmPath", "BGM_EXT", g_externalOggPath, sizeof(g_externalOggPath), g_configPath);
    ClampConfig();

    LogLine("ExternalOGG config: enabled=%s path=%s loop=%s bufferCount=%lu bufferMs=%lu fadeOutMs=%lu",
            MMX3BoolText(g_externalOggEnabled), g_externalOggPath, MMX3BoolText(g_externalOggLoop),
            (unsigned long)g_externalOggBufferCount, (unsigned long)g_externalOggBufferMs,
            (unsigned long)g_externalOggFadeOutMs);

    if (!g_externalOggEnabled) {
        LogLine("ExternalOGG disabled by MMX3.conf");
        return;
    }

    if (!PatchIAT(exe, "WINMM.DLL", "mmioOpenA", (void *)HookMmioOpenA, (void **)&g_realMmioOpenA)) {
        LogLine("ExternalOGG: PatchIAT mmioOpenA failed");
        return;
    }
    if (!PatchIAT(exe, "WINMM.DLL", "mmioClose", (void *)HookMmioClose, (void **)&g_realMmioClose)) {
        LogLine("ExternalOGG: PatchIAT mmioClose failed; fadeout stop sync unavailable");
    } else {
        LogLine("ExternalOGG: mmioClose hook installed real=%p", g_realMmioClose);
    }
    LogLine("InstallExternalOggBgmHooks installed real=%p", g_realMmioOpenA);
}
