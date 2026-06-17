#include "mmx3_common.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <string.h>
#include <process.h>
#include <stdlib.h>
#include <intrin.h>
#include <stdarg.h>
#include <math.h>
#pragma intrinsic(_ReturnAddress)

#define STB_VORBIS_HEADER_ONLY
extern "C" {
#include "third_party/stb_vorbis.c"
}

// ============================================================
// External OGG BGM fake DirectSoundBuffer bridge clean v22 OGG LOOPSTART/LOOPEND support; native SetVolume drives fades
//
// This version is intentionally based on a clean GitHub tree.
// It does not use mmioOpenA/mmioClose silent stream bridging.
// This version gives the game a fake IDirectSoundBuffer object instead of
// a real dummy DirectSoundBuffer or a silent mmio stream. The original game
// still calls SetVolume/SetPan/Play/Stop/Release through the normal buffer
// vtable, but those calls are bridged directly into the external OGG player.
// It only hooks:
//   004E14A0 Sound_RegisterStreamingWaveFile(path, soundId)
//   004DE0D0 Game_SetSleepPauseStateAndWindowTitle(this,...)
// The original Sound_PlaySlot and Sound_ReleaseSlot are left intact.
// ============================================================

static const DWORD kFnRegisterStreamingWave = 0x000E14A0;
static const DWORD kFnPlaySlot              = 0x000E0BE0;
static const DWORD kFnReleaseSlot           = 0x000E0780;
static const DWORD kFnSleepPauseState       = 0x000DE0D0;
static const DWORD kFnActivateResumeAll     = 0x000E0E30;
// 00403AC0 hook intentionally unused; the final Sleep gate is handled at 004DE0D0.

static const DWORD kSoundSystemModeAddr     = 0x005140E8;
static const DWORD kSoundSystemDirtyAddr    = 0x005140F4;
static const DWORD kSoundSystemLoadedAddr   = 0x005140F8;
static const DWORD kDirectSoundPtrAddr        = 0x005140FC;

static const DWORD kSoundSlotBaseAddr       = 0x0055E768;
static const DWORD kSoundSlotSize           = 0x178;
static const DWORD kSoundSlotCount          = 0x100;

static const DWORD kSlotLoaded              = 0x000;
static const DWORD kSlotResourceHandle      = 0x004;
static const DWORD kSlotResourcePtr         = 0x008;
static const DWORD kSlotSourceType          = 0x00C;
static const DWORD kSlotPath                = 0x010;
static const DWORD kSlotWasPlaying          = 0x114;
static const DWORD kSlotVolume              = 0x118;
static const DWORD kSlotPan                 = 0x11C;
static const DWORD kSlotFrequency           = 0x120;
static const DWORD kSlotBufferCount         = 0x124;
static const DWORD kSlotNextBufferIndex     = 0x128;
static const DWORD kSlotPlayParam           = 0x12C;
static const DWORD kSlotBuffers             = 0x130;
static const DWORD kSlotStreamingFlag       = 0x170;
static const DWORD kSlotStreamPtr           = 0x174;

struct ExternalOggSlot {
    BOOL active;
    BOOL hasOgg;
    char nativePath[MAX_PATH];
    char oggPath[MAX_PATH];
};


static ExternalOggSlot g_slots[kSoundSlotCount];

// Fake WaveStream metadata used only so the original resume-all path treats
// external OGG slots like native streaming WAV slots. The original Sound_PlaySlot
// writes stream+0x48 with the logical loop flag before it calls IDirectSoundBuffer::Play.
static BYTE g_dummyStreams[kSoundSlotCount][0x50];
static unsigned int g_resumeSampleOffset[kSoundSlotCount];
static volatile LONG g_deactivateResumePendingSoundId = -1;
static volatile LONG g_currentSampleOffset = 0;
static volatile LONG g_currentVolume = 0;  // DirectSound volume, hundredths of dB, 0..-10000
static volatile LONG g_currentPan = 0;     // DirectSound pan, hundredths of dB, -10000..10000

static BOOL g_enabled = TRUE;
static BOOL g_replaceNativeStream = TRUE;
#ifdef _DEBUG
static BOOL g_verboseLog = TRUE;
#else
static BOOL g_verboseLog = FALSE;
#endif
static BOOL g_pauseWithGameSleep = TRUE;
static char g_externalBgmPath[MAX_PATH] = "BGM_EXT";
static int g_bufferCount = 3;
static int g_bufferMs = 60;

static CRITICAL_SECTION g_lock;
static INIT_ONCE g_lockInitOnce = INIT_ONCE_STATIC_INIT;
static volatile LONG g_gameSleepPauseActive = 0;
static volatile LONG g_userSleepHold = 0;
static volatile LONG g_sleepBlockedPlaySoundId = -1;
static BOOL g_sleepBlockedPlayLoop = FALSE;
static volatile LONG g_externalPaused = 0;
static volatile LONG g_stopRequested = 0;
static volatile LONG g_naturalEnded = 0;
static volatile LONG g_outputStopped = 0;

// Fake IDirectSoundBuffer::Stop is also used by the original game while
// entering Sleep/Pause.  If we treat that Stop as a real BGM stop immediately,
// Sleep fades the music out before the game state hook has a chance to pause it.
// Keep Stop pending briefly. If Sleep/Pause follows, mark that Stop as
// sleep-related and cancel it on resume, so focus changes do not fade BGM out.
static volatile LONG g_pendingFakeStop = 0;
static volatile LONG g_pendingFakeStopSoundId = -1;
static volatile LONG g_pendingFakeStopSuspendedBySleep = 0;
static DWORD g_pendingFakeStopTick = 0;
static const DWORD kPendingFakeStopGuardMs = 50;

// v12: The original DirectSound layer often emits a very short Stop/ResumeAll
// pulse during internal active/display transitions. A real DirectSound buffer
// absorbs that cheaply, but waveOut reset/refill makes it audible. Delay the
// deactivate Stop briefly; if Play comes back quickly, swallow the pulse.
static volatile LONG g_pendingDeactivateStop = 0;
static volatile LONG g_pendingDeactivateStopSoundId = -1;
static DWORD g_pendingDeactivateStopTick = 0;
static unsigned int g_pendingDeactivateStopSample = 0;
static const DWORD kPendingDeactivateStopGuardMs = 100;

// The original game may briefly report active state immediately after it enters
// Sleep/Pause. Do not restart waveOut on that transient state. Delay resume and
// require the game window/process to be foreground before waveOutRestart.
static volatile LONG g_delayedGameResume = 0;
static DWORD g_delayedGameResumeTick = 0;
static HWND g_delayedGameResumeHwnd = NULL;
static const DWORD kDelayedGameResumeMs = 250;

static HANDLE g_thread = NULL;
static HANDLE g_stopEvent = NULL;
static HANDLE g_waveEvent = NULL;
static HWAVEOUT g_waveOut = NULL;
static int g_currentSoundId = -1;
static BOOL g_currentLoop = FALSE;
static char g_currentOggPath[MAX_PATH] = "";

struct OggThreadParam {
    char path[MAX_PATH];
    BOOL loop;
    int bufferCount;
    int bufferMs;
    int soundId;
    unsigned int startSample;
};

struct OggLoopInfo {
    BOOL enabled;
    unsigned int start;
    unsigned int end;     // exclusive sample index
    unsigned int length;
};

typedef int (__cdecl *RegisterStreamingWaveFn)(LPCSTR path, int soundId);
typedef int (__cdecl *PlaySlotFn)(int soundId, int loopFlag);
typedef void (__cdecl *ReleaseSlotFn)(int soundId);
typedef void (__thiscall *GameSleepPauseStateFn)(int *self, int param2, int param3);
typedef void (__cdecl *ActivateResumeAllFn)();

static RegisterStreamingWaveFn g_realRegisterStreamingWave = NULL;
static PlaySlotFn g_realPlaySlot = NULL;
static ReleaseSlotFn g_realReleaseSlot = NULL;
static GameSleepPauseStateFn g_realSleepPauseState = NULL;
static ActivateResumeAllFn g_realActivateResumeAll = NULL;

// v18: The simple gateway trampoline is not safe for Sound_RegisterStreamingWave
// fallback because the stolen prologue may contain PC-relative instructions.
// For native fallback we temporarily restore the original bytes, call the
// function in-place, then reinstall the hook. This keeps relative operands valid.
static BYTE *g_registerTarget = NULL;
static BYTE g_registerOriginalBytes[16];
static SIZE_T g_registerStolen = 0;
static volatile LONG g_registerNativeCallDepth = 0;

struct FakeDirectSoundBuffer;
static BOOL StartExternalOggPlayback(int soundId, const char *path, BOOL loop);
static void RequestExternalOggStopNow();
static LONG ClampDsbVolume(LONG volume);
static LONG ClampDsbPan(LONG pan);
static void HardStopExternalOggPlayback();
static void StopExternalOggOutputForDeactivate(int soundId);
static void RequestPendingDeactivateStop(int soundId, unsigned int saveSample);
static BOOL CancelPendingDeactivateStopForSound(int soundId, const char *reason);
static void FlushPendingDeactivateStopIfExpired();
static void ForceSlotWasPlayingBeforeDeactivate(int soundId, const char *reason);
static void CancelPendingFakeStop(const char *reason);
static void FlushPendingFakeStopIfExpired();
static void PauseExternalOggPlayback();
static void ResumeExternalOggPlayback();
static void CancelDelayedGameResume(const char *reason);
static void RequestDelayedGameResume(HWND hwnd);
static void FlushDelayedGameResumeIfReady();
static void ResumeExternalOggSlotsAfterActivate();
static void TraceWindowState(const char *tag, int *self);
static void ResumeBlockedPlayAfterUserSleepHold();
static int __cdecl HookRegisterStreamingWave(LPCSTR path, int soundId);
static int CallOriginalRegisterStreamingWaveNative(LPCSTR path, int soundId);

static BOOL CALLBACK InitOggLockOnce(PINIT_ONCE, PVOID, PVOID *)
{
    InitializeCriticalSection(&g_lock);
    g_stopEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
    g_waveEvent = CreateEventA(NULL, FALSE, FALSE, NULL);
    return TRUE;
}

static void EnsureLock()
{
    InitOnceExecuteOnce(&g_lockInitOnce, InitOggLockOnce, NULL, NULL);
}


static void OggTrace(const char *fmt, ...)
{
    if (!g_verboseLog) {
        return;
    }

    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    va_end(ap);
    buf[sizeof(buf) - 1] = 0;
    LogLine("%s", buf);
}

static void CancelPendingFakeStop(const char *reason)
{
    if (InterlockedExchange(&g_pendingFakeStop, 0) != 0) {
        LONG soundId = InterlockedCompareExchange(&g_pendingFakeStopSoundId, -1, -1);
        InterlockedExchange(&g_pendingFakeStopSuspendedBySleep, 0);
        OggTrace("ExternalOGG: pending fake DS Stop canceled soundId=%ld reason=%s",
                soundId, reason ? reason : "unknown");
    }
}


static void RequestPendingDeactivateStop(int soundId, unsigned int saveSample)
{
    g_pendingDeactivateStopSample = saveSample;
    g_pendingDeactivateStopTick = timeGetTime();
    InterlockedExchange(&g_pendingDeactivateStopSoundId, soundId);
    InterlockedExchange(&g_pendingDeactivateStop, 1);
    OggTrace("ExternalOGG V15: pending deactivate stop armed soundId=%d guardMs=%lu sample=%u",
            soundId, (unsigned long)kPendingDeactivateStopGuardMs, saveSample);
}

static BOOL CancelPendingDeactivateStopForSound(int soundId, const char *reason)
{
    if (InterlockedCompareExchange(&g_pendingDeactivateStop, 0, 0) == 0) {
        return FALSE;
    }
    LONG pendingSoundId = InterlockedCompareExchange(&g_pendingDeactivateStopSoundId, -1, -1);
    if (pendingSoundId != soundId) {
        return FALSE;
    }
    InterlockedExchange(&g_pendingDeactivateStop, 0);
    InterlockedExchange(&g_pendingDeactivateStopSoundId, -1);
    if (InterlockedCompareExchange(&g_deactivateResumePendingSoundId, -1, -1) == soundId) {
        InterlockedExchange(&g_deactivateResumePendingSoundId, -1);
    }
    OggTrace("ExternalOGG V15: pending deactivate stop canceled soundId=%d reason=%s",
            soundId, reason ? reason : "unknown");
    return TRUE;
}

static void FlushPendingDeactivateStopIfExpired()
{
    if (InterlockedCompareExchange(&g_pendingDeactivateStop, 0, 0) == 0) {
        return;
    }
    DWORD now = timeGetTime();
    if ((DWORD)(now - g_pendingDeactivateStopTick) < kPendingDeactivateStopGuardMs) {
        return;
    }
    LONG soundId = InterlockedExchange(&g_pendingDeactivateStopSoundId, -1);
    unsigned int sample = g_pendingDeactivateStopSample;
    if (InterlockedExchange(&g_pendingDeactivateStop, 0) != 0) {
        OggTrace("ExternalOGG V15: pending deactivate stop expired soundId=%ld -> waveOutReset sample=%u",
                soundId, sample);
        if (soundId >= 0 && soundId < (LONG)kSoundSlotCount) {
            g_resumeSampleOffset[soundId] = sample;
            InterlockedExchange(&g_deactivateResumePendingSoundId, soundId);
            StopExternalOggOutputForDeactivate((int)soundId);
        }
    }
}

static void CancelDelayedGameResume(const char *reason)
{
    if (InterlockedExchange(&g_delayedGameResume, 0) != 0) {
        OggTrace("ExternalOGG: delayed game resume canceled reason=%s", reason ? reason : "unknown");
    }
}

static void RequestDelayedGameResume(HWND hwnd)
{
    // Only delay if we actually have a paused waveOut stream. If there is no
    // external audio active, this request is harmless but noisy.
    if (InterlockedCompareExchange(&g_externalPaused, 0, 0) == 0) {
        return;
    }

    g_delayedGameResumeHwnd = hwnd;
    g_delayedGameResumeTick = timeGetTime() + kDelayedGameResumeMs;
    InterlockedExchange(&g_delayedGameResume, 1);
    OggTrace("ExternalOGG: delayed game resume requested hwnd=%p ms=%lu", hwnd, (unsigned long)kDelayedGameResumeMs);
}

static BOOL IsGameWindowForeground(HWND hwnd)
{
    if (!hwnd) {
        return TRUE;
    }

    HWND fg = GetForegroundWindow();
    if (!fg) {
        return FALSE;
    }
    if (fg == hwnd) {
        return TRUE;
    }

    DWORD fgPid = 0;
    GetWindowThreadProcessId(fg, &fgPid);
    return fgPid == GetCurrentProcessId();
}

static void FlushDelayedGameResumeIfReady()
{
    if (InterlockedCompareExchange(&g_delayedGameResume, 0, 0) == 0) {
        return;
    }
    if (InterlockedCompareExchange(&g_gameSleepPauseActive, 0, 0) != 0) {
        return;
    }

    DWORD now = timeGetTime();
    if ((LONG)(now - g_delayedGameResumeTick) < 0) {
        return;
    }

    HWND hwnd = g_delayedGameResumeHwnd;
    if (!IsGameWindowForeground(hwnd)) {
        // Keep waiting while the game is not actually foreground. This prevents
        // the transient new=(0,0) state during Alt-Tab from restarting music.
        return;
    }

    if (InterlockedExchange(&g_delayedGameResume, 0) != 0) {
        OggTrace("ExternalOGG: delayed game resume accepted hwnd=%p", hwnd);
        ResumeExternalOggPlayback();
    }
}


static void MarkPendingFakeStopSuspendedBySleep(void)
{
    if (InterlockedCompareExchange(&g_pendingFakeStop, 0, 0) != 0) {
        LONG soundId = InterlockedCompareExchange(&g_pendingFakeStopSoundId, -1, -1);
        if (InterlockedExchange(&g_pendingFakeStopSuspendedBySleep, 1) == 0) {
            OggTrace("ExternalOGG: pending fake DS Stop suspended by game sleep/pause soundId=%ld", soundId);
        }
    }
}

static void FlushPendingFakeStopIfExpired()
{
    if (InterlockedCompareExchange(&g_pendingFakeStop, 0, 0) == 0) {
        return;
    }

    // While the original game is in Sleep/Pause, a Stop immediately before the
    // state transition is only DirectSound housekeeping. Do not turn it into a
    // real BGM stop here. The Sleep/Pause hook will cancel it on resume.
    if (InterlockedCompareExchange(&g_gameSleepPauseActive, 0, 0) != 0) {
        return;
    }

    DWORD now = timeGetTime();
    if ((DWORD)(now - g_pendingFakeStopTick) < kPendingFakeStopGuardMs) {
        return;
    }

    LONG soundId = InterlockedCompareExchange(&g_pendingFakeStopSoundId, -1, -1);
    if (InterlockedExchange(&g_pendingFakeStop, 0) != 0) {
        InterlockedExchange(&g_pendingFakeStopSuspendedBySleep, 0);
        OggTrace("ExternalOGG: pending fake DS Stop expired soundId=%ld -> fadeout", soundId);
        RequestExternalOggStopNow();
    }
}

static BOOL FileExistsA2(const char *path)
{
    DWORD attr = GetFileAttributesA(path);
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

static void GetBaseNameNoExt(const char *path, char *out, size_t outSize)
{
    if (!out || outSize == 0) return;
    out[0] = 0;
    if (!path) return;
    const char *base = strrchr(path, '\\');
    const char *base2 = strrchr(path, '/');
    if (!base || (base2 && base2 > base)) base = base2;
    base = base ? base + 1 : path;
    lstrcpynA(out, base, (int)outSize);
    char *dot = strrchr(out, '.');
    if (dot) *dot = 0;
}

static BOOL IsBgmSePath(const char *path)
{
    if (!path || !path[0]) return FALSE;
    const char *ext = strrchr(path, '.');
    if (!ext || _stricmp(ext, ".SE") != 0) return FALSE;
    return (strstr(path, "\\BGM\\") || strstr(path, "/BGM/")) ? TRUE : FALSE;
}

static BOOL BuildExternalOggPath(const char *nativePath, char *out, size_t outSize)
{
    if (!nativePath || !out || outSize == 0) return FALSE;
    char base[64];
    GetBaseNameNoExt(nativePath, base, sizeof(base));
    if (!base[0]) return FALSE;

    lstrcpynA(out, g_gameDir, (int)outSize);
    if (out[0] && out[lstrlenA(out) - 1] != '\\') lstrcatA(out, "\\");
    lstrcatA(out, g_externalBgmPath);
    if (out[0] && out[lstrlenA(out) - 1] != '\\') lstrcatA(out, "\\");
    lstrcatA(out, base);
    lstrcatA(out, ".ogg");
    return TRUE;
}

static void EnsureIniString(const char *section, const char *key, const char *value)
{
    char buf[256];
    buf[0] = 0;
    GetPrivateProfileStringA(section, key, "", buf, sizeof(buf), g_configPath);
    if (!buf[0]) {
        WritePrivateProfileStringA(section, key, value, g_configPath);
        LogLine("ExternalOGG: wrote default %s.%s=%s", section, key, value);
    }
}

static int GetIniInt(const char *section, const char *key, int defValue)
{
    char defText[32];
    wsprintfA(defText, "%d", defValue);
    char buf[64];
    GetPrivateProfileStringA(section, key, defText, buf, sizeof(buf), g_configPath);
    return atoi(buf);
}

static void EnsureAudioDefaults()
{
    EnsureIniString("Audio", "ExternalOggBgm", "True");
    EnsureIniString("Audio", "ExternalBgmPath", "BGM_EXT");
    EnsureIniString("Audio", "ExternalOggBufferCount", "3");
    EnsureIniString("Audio", "ExternalOggBufferMs", "15");
    EnsureIniString("Audio", "ExternalOggPauseWithGameSleep", "True");
    EnsureIniString("Audio", "ExternalOggReplaceNativeStream", "True");
}

static BYTE *SlotPtr(int soundId)
{
    if (soundId < 0 || soundId >= (int)kSoundSlotCount) return NULL;
    return (BYTE *)(kSoundSlotBaseAddr + soundId * kSoundSlotSize);
}


static BOOL IsDeactivateStopCaller(DWORD callerRet)
{
    // FUN_004E0D10 walks every DirectSound buffer during deactivate/sleep,
    // records slot+0x114 when a buffer was playing, then calls
    // IDirectSoundBuffer::Stop and SetCurrentPosition(0).  The Stop return
    // address observed from the game is 004E0DF7. Treat the whole function
    // range as DirectSound housekeeping, not as a real BGM stop.
    return callerRet >= 0x004E0D10 && callerRet <= 0x004E0E20;
}

static BOOL IsReleaseStopCaller(DWORD callerRet)
{
    // Observed release cleanup Stop return address from Sound_ReleaseSlot.
    // Release() follows immediately and owns the real fade/cleanup.
    return callerRet >= 0x004E0780 && callerRet <= 0x004E0820;
}


struct FakeDirectSoundBuffer {
    void **vtable;
    LONG refCount;
    int soundId;
    LONG playing;
    LONG volume;
    LONG pan;
    DWORD frequency;
    char oggPath[MAX_PATH];
};

static HRESULT __stdcall FakeDSB_QueryInterface(FakeDirectSoundBuffer *self, REFIID riid, void **out)
{
    (void)self;
    (void)riid;
    if (out) *out = NULL;
    return E_NOINTERFACE;
}

static ULONG __stdcall FakeDSB_AddRef(FakeDirectSoundBuffer *self)
{
    if (!self) return 0;
    return (ULONG)InterlockedIncrement(&self->refCount);
}

static ULONG __stdcall FakeDSB_Release(FakeDirectSoundBuffer *self)
{
    if (!self) return 0;
    LONG ref = InterlockedDecrement(&self->refCount);
    LogLine("ExternalOGG: fake DS buffer Release soundId=%d ref=%ld", self->soundId, ref);
    if (ref <= 0) {
        if (g_currentSoundId == self->soundId) {
            CancelPendingFakeStop("fake DS Release");
            RequestExternalOggStopNow();
        }
        GlobalFree(self);
        return 0;
    }
    return (ULONG)ref;
}

static HRESULT __stdcall FakeDSB_GetCaps(FakeDirectSoundBuffer *self, void *caps)
{
    (void)self;
    if (caps) ZeroMemory(caps, 4);
    return 0;
}

static HRESULT __stdcall FakeDSB_GetCurrentPosition(FakeDirectSoundBuffer *self, DWORD *play, DWORD *write)
{
    (void)self;
    if (play) *play = 0;
    if (write) *write = 0;
    return 0;
}

static HRESULT __stdcall FakeDSB_GetFormat(FakeDirectSoundBuffer *self, WAVEFORMATEX *format, DWORD size, DWORD *written)
{
    (void)self;
    if (written) *written = sizeof(WAVEFORMATEX);
    if (format && size >= sizeof(WAVEFORMATEX)) {
        ZeroMemory(format, sizeof(WAVEFORMATEX));
        format->wFormatTag = WAVE_FORMAT_PCM;
        format->nChannels = 2;
        format->nSamplesPerSec = 44100;
        format->wBitsPerSample = 16;
        format->nBlockAlign = 4;
        format->nAvgBytesPerSec = 176400;
        if (written) *written = sizeof(WAVEFORMATEX);
    }
    return 0;
}

static HRESULT __stdcall FakeDSB_GetVolume(FakeDirectSoundBuffer *self, LONG *volume)
{
    if (volume) *volume = self ? self->volume : 0;
    return 0;
}

static HRESULT __stdcall FakeDSB_GetPan(FakeDirectSoundBuffer *self, LONG *pan)
{
    if (pan) *pan = self ? self->pan : 0;
    return 0;
}

static HRESULT __stdcall FakeDSB_GetFrequency(FakeDirectSoundBuffer *self, DWORD *freq)
{
    if (freq) *freq = self ? self->frequency : 44100;
    return 0;
}

static HRESULT __stdcall FakeDSB_GetStatus(FakeDirectSoundBuffer *self, DWORD *status)
{
    void *caller = _ReturnAddress();
    DWORD outStatus = 0;
    LONG selfPlaying = self ? InterlockedCompareExchange(&self->playing, 0, 0) : 0;
    BOOL currentThreadAlive = FALSE;
    EnsureLock();
    EnterCriticalSection(&g_lock);
    currentThreadAlive = (self && g_thread != NULL && g_currentSoundId == self->soundId &&
                          InterlockedCompareExchange(&g_naturalEnded, 0, 0) == 0);
    LeaveCriticalSection(&g_lock);

    // DirectSound's deactivate path at 004E0D10 uses GetStatus() to decide
    // whether to set slot+0x114 (was-playing-before-deactivate). For the fake
    // buffer, logical playing means either the fake object was Play()'d or the
    // external OGG thread is currently bound to this sound slot.
    if (self && (selfPlaying != 0 || currentThreadAlive)) {
        outStatus = 1; // DSBSTATUS_PLAYING
    }
    if (status) *status = outStatus;

    OggTrace("ExternalOGG V15 trace: fake DS GetStatus callerRet=%p soundId=%d -> status=0x%08lX selfPlaying=%ld currentSoundId=%d thread=%p naturalEnded=%ld",
            caller, self ? self->soundId : -1, (unsigned long)outStatus, selfPlaying,
            g_currentSoundId, g_thread, InterlockedCompareExchange(&g_naturalEnded, 0, 0));
    return 0;
}

static HRESULT __stdcall FakeDSB_Initialize(FakeDirectSoundBuffer *self, void *ds, void *desc)
{
    (void)self;
    (void)ds;
    (void)desc;
    return 0;
}

static HRESULT __stdcall FakeDSB_Lock(FakeDirectSoundBuffer *self, DWORD offset, DWORD bytes, void **p1, DWORD *b1, void **p2, DWORD *b2, DWORD flags)
{
    (void)self; (void)offset; (void)bytes; (void)flags;
    if (p1) *p1 = NULL;
    if (b1) *b1 = 0;
    if (p2) *p2 = NULL;
    if (b2) *b2 = 0;
    return 0;
}

static HRESULT __stdcall FakeDSB_Play(FakeDirectSoundBuffer *self, DWORD reserved1, DWORD priority, DWORD flags)
{
    (void)reserved1;
    (void)priority;
    if (!self) return E_FAIL;

    void *caller = _ReturnAddress();
    DWORD callerRet = (DWORD)(uintptr_t)caller;
    BOOL loop = (flags & 1) ? TRUE : FALSE; // non-streaming DirectSound semantics
    BYTE *slot = SlotPtr(self->soundId);
    if (slot) {
        __try {
            DWORD streamingFlag = *(DWORD *)(slot + kSlotStreamingFlag);
            BYTE *stream = *(BYTE **)(slot + kSlotStreamPtr);
            if (streamingFlag != 0 && stream != NULL) {
                loop = (*(DWORD *)(stream + 0x48) != 0) ? TRUE : FALSE;
            }
        } __except(EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    BOOL wasPaused = (InterlockedCompareExchange(&g_externalPaused, 0, 0) != 0);

    OggTrace("ExternalOGG trace: fake DS Play callerRet=%p soundId=%d flags=0x%08lX",
            caller, self->soundId, (unsigned long)flags);
    OggTrace("ExternalOGG: fake DS Play soundId=%d flags=0x%08lX -> start ogg=\"%s\" loop=%s",
            self->soundId, (unsigned long)flags, self->oggPath, MMX3BoolText(loop));

    if (InterlockedCompareExchange(&g_userSleepHold, 0, 0) != 0) {
        InterlockedExchange(&g_sleepBlockedPlaySoundId, self->soundId);
        g_sleepBlockedPlayLoop = loop;
        InterlockedExchange(&self->playing, 0);
        CancelPendingFakeStop("fake DS Play blocked by user sleep hold");
        CancelPendingDeactivateStopForSound(self->soundId, "fake DS Play blocked by user sleep hold");
        OggTrace("ExternalOGG V15: fake DS Play blocked by user sleep hold soundId=%d callerRet=%p path=\"%s\" loop=%s",
                self->soundId, caller, self->oggPath, MMX3BoolText(loop));
        return 0;
    }

    CancelPendingFakeStop("fake DS Play");

    InterlockedExchange(&g_currentVolume, ClampDsbVolume(self->volume));
    InterlockedExchange(&g_currentPan, ClampDsbPan(self->pan));

    if (StartExternalOggPlayback(self->soundId, self->oggPath, loop)) {
        InterlockedExchange(&self->playing, 1);
        // In v12: game active/title state is not trusted as a resume source.
        // Resume/restart only when the original sound layer actually calls Play again.
        if (wasPaused) {
            OggTrace("ExternalOGG: fake DS Play callerRet=%p resumes suspended waveOut soundId=%d", caller, self->soundId);
            ResumeExternalOggPlayback();
        }
        return 0;
    }
    return E_FAIL;
}

static HRESULT __stdcall FakeDSB_SetCurrentPosition(FakeDirectSoundBuffer *self, DWORD pos)
{
    void *caller = _ReturnAddress();
    DWORD callerRet = (DWORD)(uintptr_t)caller;
    if (IsDeactivateStopCaller(callerRet) && pos == 0) {
        // Original deactivate path calls Stop() then SetCurrentPosition(0).
        // In v12 Stop() enters a persistent stopped DirectSound-like state, so this
        // position reset is accepted as a no-op on the fake buffer object.
        OggTrace("ExternalOGG trace: fake DS SetCurrentPosition callerRet=%p soundId=%d pos=%lu accepted deactivate reset",
                caller, self ? self->soundId : -1, (unsigned long)pos);
        return 0;
    }
    OggTrace("ExternalOGG trace: fake DS SetCurrentPosition callerRet=%p soundId=%d pos=%lu ignored",
            caller, self ? self->soundId : -1, (unsigned long)pos);
    return 0;
}

static HRESULT __stdcall FakeDSB_SetFormat(FakeDirectSoundBuffer *self, const WAVEFORMATEX *format)
{
    (void)self;
    (void)format;
    return 0;
}

static HRESULT __stdcall FakeDSB_SetPan(FakeDirectSoundBuffer *self, LONG pan)
{
    void *caller = _ReturnAddress();
    pan = ClampDsbPan(pan);
    if (self) {
        self->pan = pan;
        if (g_currentSoundId == self->soundId || InterlockedCompareExchange(&self->playing, 0, 0) != 0) {
            InterlockedExchange(&g_currentPan, pan);
        }
        OggTrace("ExternalOGG V20: fake DS SetPan callerRet=%p soundId=%d pan=%ld",
                caller, self->soundId, pan);
    }
    return 0;
}

static HRESULT __stdcall FakeDSB_SetVolume(FakeDirectSoundBuffer *self, LONG volume)
{
    void *caller = _ReturnAddress();
    volume = ClampDsbVolume(volume);
    if (self) {
        self->volume = volume;
        if (g_currentSoundId == self->soundId || InterlockedCompareExchange(&self->playing, 0, 0) != 0) {
            InterlockedExchange(&g_currentVolume, volume);
        }
        OggTrace("ExternalOGG V20: fake DS SetVolume callerRet=%p soundId=%d volume=%ld",
                caller, self->soundId, volume);
    }
    return 0;
}

static HRESULT __stdcall FakeDSB_SetFrequency(FakeDirectSoundBuffer *self, DWORD freq)
{
    if (self) self->frequency = freq;
    return 0;
}

static HRESULT __stdcall FakeDSB_Stop(FakeDirectSoundBuffer *self)
{
    if (self) {
        void *caller = _ReturnAddress();
        DWORD callerRet = (DWORD)(uintptr_t)caller;
        OggTrace("ExternalOGG trace: fake DS Stop callerRet=%p soundId=%d", caller, self->soundId);

        LONG wasSelfPlaying = InterlockedCompareExchange(&self->playing, 0, 0);
        BOOL wasCurrentOgg = (g_currentSoundId == self->soundId && g_thread != NULL &&
                              InterlockedCompareExchange(&g_naturalEnded, 0, 0) == 0);
        InterlockedExchange(&self->playing, 0);

        if (IsDeactivateStopCaller(callerRet)) {
            // Stop from FUN_004E0D10 is the original game's deactivate/sleep
            // StopAll path. It immediately follows with SetCurrentPosition(0), so
            // the faithful behavior is a hard stop/reset, not waveOutPause.
            // If the original game wants this sound again, it will call Play()
            // through the normal sound layer and we will start the OGG again.
            if (wasSelfPlaying != 0 || wasCurrentOgg) {
                ForceSlotWasPlayingBeforeDeactivate(self->soundId, "deactivate stop-all");
            }
            if (g_currentSoundId == self->soundId) {
                OggTrace("ExternalOGG V15: fake DS Stop soundId=%d callerRet=%p -> PENDING DEACTIVATE STOP (deactivate stop-all) wasSelfPlaying=%ld wasCurrentOgg=%s",
                        self->soundId, caller, wasSelfPlaying, MMX3BoolText(wasCurrentOgg));
                unsigned int saveSample = (unsigned int)InterlockedCompareExchange(&g_currentSampleOffset, 0, 0);
                RequestPendingDeactivateStop(self->soundId, saveSample);
            } else {
                OggTrace("ExternalOGG V15: fake DS Stop soundId=%d callerRet=%p deactivate ignored because currentSoundId=%d wasSelfPlaying=%ld wasCurrentOgg=%s",
                        self->soundId, caller, g_currentSoundId, wasSelfPlaying, MMX3BoolText(wasCurrentOgg));
            }
            return 0;
        }

        if (IsReleaseStopCaller(callerRet)) {
            // Release path immediately calls FakeDSB_Release, which performs the
            // real fade/cleanup. Avoid double-pending or treating this as sleep.
            CancelPendingDeactivateStopForSound(self->soundId, "release stop");
            CancelPendingFakeStop("release stop");
            OggTrace("ExternalOGG: fake DS Stop soundId=%d callerRet=%p -> release-path stop ignored", self->soundId, caller);
            return 0;
        }

        if (g_currentSoundId == self->soundId) {
            g_pendingFakeStopTick = timeGetTime();
            InterlockedExchange(&g_pendingFakeStopSoundId, self->soundId);
            InterlockedExchange(&g_pendingFakeStopSuspendedBySleep, 0);
            InterlockedExchange(&g_pendingFakeStop, 1);
            OggTrace("ExternalOGG: fake DS Stop soundId=%d callerRet=%p -> pending stop", self->soundId, caller);
        } else {
            OggTrace("ExternalOGG: fake DS Stop soundId=%d callerRet=%p ignored because currentSoundId=%d",
                    self->soundId, caller, g_currentSoundId);
        }
    }
    return 0;
}

static HRESULT __stdcall FakeDSB_Unlock(FakeDirectSoundBuffer *self, void *p1, DWORD b1, void *p2, DWORD b2)
{
    (void)self; (void)p1; (void)b1; (void)p2; (void)b2;
    return 0;
}

static HRESULT __stdcall FakeDSB_Restore(FakeDirectSoundBuffer *self)
{
    (void)self;
    return 0;
}

static void *g_fakeDsBufferVTable[] = {
    (void *)FakeDSB_QueryInterface,
    (void *)FakeDSB_AddRef,
    (void *)FakeDSB_Release,
    (void *)FakeDSB_GetCaps,
    (void *)FakeDSB_GetCurrentPosition,
    (void *)FakeDSB_GetFormat,
    (void *)FakeDSB_GetVolume,
    (void *)FakeDSB_GetPan,
    (void *)FakeDSB_GetFrequency,
    (void *)FakeDSB_GetStatus,
    (void *)FakeDSB_Initialize,
    (void *)FakeDSB_Lock,
    (void *)FakeDSB_Play,
    (void *)FakeDSB_SetCurrentPosition,
    (void *)FakeDSB_SetFormat,
    // IDirectSoundBuffer vtable order is SetVolume at +0x3C, then SetPan at +0x40.
    // Earlier builds had these two swapped, so game SetVolume calls were treated as pan.
    (void *)FakeDSB_SetVolume,
    (void *)FakeDSB_SetPan,
    (void *)FakeDSB_SetFrequency,
    (void *)FakeDSB_Stop,
    (void *)FakeDSB_Unlock,
    (void *)FakeDSB_Restore,
};

static BOOL CreateFakeDirectSoundBuffer(int soundId, const char *oggPath, void **outBuffer)
{
    if (!outBuffer) return FALSE;
    *outBuffer = NULL;
    FakeDirectSoundBuffer *fake = (FakeDirectSoundBuffer *)GlobalAlloc(GPTR, sizeof(FakeDirectSoundBuffer));
    if (!fake) return FALSE;
    fake->vtable = g_fakeDsBufferVTable;
    fake->refCount = 1;
    fake->soundId = soundId;
    fake->playing = 0;
    fake->volume = 0;
    fake->pan = 0;
    fake->frequency = 44100;
    lstrcpynA(fake->oggPath, oggPath ? oggPath : "", sizeof(fake->oggPath));
    *outBuffer = fake;
    LogLine("ExternalOGG: fake DS buffer created soundId=%d object=%p ogg=\"%s\"", soundId, fake, fake->oggPath);
    return TRUE;
}

static BOOL MarkExternalSlotRegistered(int soundId, const char *nativePath, void *fakeBuffer)
{
    BYTE *slot = SlotPtr(soundId);
    if (!slot || !fakeBuffer) return FALSE;

    *(DWORD *)(slot + kSlotLoaded) = 1;
    *(DWORD *)(slot + kSlotResourceHandle) = 0;
    *(DWORD *)(slot + kSlotResourcePtr) = 0;
    *(DWORD *)(slot + kSlotSourceType) = 1;
    *(DWORD *)(slot + kSlotVolume) = 0;
    *(DWORD *)(slot + kSlotPan) = 0;
    *(DWORD *)(slot + kSlotFrequency) = 0;
    *(DWORD *)(slot + kSlotBufferCount) = 1;
    *(DWORD *)(slot + kSlotNextBufferIndex) = 0;
    *(DWORD *)(slot + kSlotPlayParam) = 0;
    for (int i = 0; i < 16; ++i) {
        *(DWORD *)(slot + kSlotBuffers + i * 4) = 0;
    }
    *(void **)(slot + kSlotBuffers) = fakeBuffer;

    // v12: make the fake OGG slot look like a native streaming slot to the
    // original resume-all function (004E0E30). This lets the game naturally call
    // Sound_PlaySlot on activate instead of us manually calling the fake buffer.
    // The dummy stream is only metadata; Sound_PlaySlot writes stream+0x48 with
    // the logical loop flag. FakeDSB_Play reads it back so non-loop tracks do not
    // become looping just because DirectSound Play receives DSBPLAY_LOOPING.
    ZeroMemory(g_dummyStreams[soundId], sizeof(g_dummyStreams[soundId]));
    *(DWORD *)(slot + kSlotStreamingFlag) = 1;
    *(DWORD *)(slot + kSlotStreamPtr) = (DWORD)(uintptr_t)&g_dummyStreams[soundId][0];
    lstrcpynA((char *)(slot + kSlotPath), nativePath ? nativePath : "", 256);

    *(DWORD *)kSoundSystemDirtyAddr = 1;
    if (*(DWORD *)kSoundSystemLoadedAddr == 0) {
        *(DWORD *)kSoundSystemLoadedAddr = 1;
    }
    return TRUE;
}

static void ClearExternalSlot(int soundId)
{
    BYTE *slot = SlotPtr(soundId);
    if (!slot) return;
    *(DWORD *)(slot + kSlotLoaded) = 0;
    *(DWORD *)(slot + kSlotResourceHandle) = 0;
    *(DWORD *)(slot + kSlotResourcePtr) = 0;
    *(DWORD *)(slot + kSlotStreamingFlag) = 0;
    *(DWORD *)(slot + kSlotStreamPtr) = 0;
    g_resumeSampleOffset[soundId] = 0;
    if (InterlockedCompareExchange(&g_deactivateResumePendingSoundId, -1, soundId) == soundId) {
        InterlockedExchange(&g_deactivateResumePendingSoundId, -1);
    }
}


static OggLoopInfo EmptyOggLoopInfo()
{
    OggLoopInfo info;
    info.enabled = FALSE;
    info.start = 0;
    info.end = 0;
    info.length = 0;
    return info;
}

static BOOL ParseU32Text(const char *text, unsigned int *out)
{
    if (!text || !out) return FALSE;

    while (*text == ' ' || *text == '\t') ++text;
    if (*text == '-') return FALSE;
    if (*text == '+') ++text;
    if (!*text) return FALSE;

    char *end = NULL;
    unsigned long value = strtoul(text, &end, 10);
    if (end == text) return FALSE;

    while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n') ++end;
    if (*end != '\0') return FALSE;

    *out = (unsigned int)value;
    return TRUE;
}

static BOOL ReadCommentU32(const char *comment, const char *key, unsigned int *out)
{
    if (!comment || !key || !out) return FALSE;

    size_t keyLen = strlen(key);
    if (_strnicmp(comment, key, keyLen) != 0) return FALSE;
    if (comment[keyLen] != '=') return FALSE;

    return ParseU32Text(comment + keyLen + 1, out);
}

static OggLoopInfo ReadOggLoopInfo(stb_vorbis *vorbis, const char *path)
{
    OggLoopInfo info = EmptyOggLoopInfo();
    if (!vorbis) return info;

    unsigned int loopStart = 0;
    unsigned int loopEnd = 0;
    unsigned int loopLength = 0;

    BOOL hasStart = FALSE;
    BOOL hasEnd = FALSE;
    BOOL hasLength = FALSE;

    stb_vorbis_comment comment = stb_vorbis_get_comment(vorbis);
    for (int i = 0; i < comment.comment_list_length; ++i) {
        const char *s = comment.comment_list ? comment.comment_list[i] : NULL;
        unsigned int value = 0;

        if (ReadCommentU32(s, "LOOPSTART", &value) ||
            ReadCommentU32(s, "LOOP_START", &value)) {
            loopStart = value;
            hasStart = TRUE;
            continue;
        }

        if (ReadCommentU32(s, "LOOPEND", &value) ||
            ReadCommentU32(s, "LOOP_END", &value)) {
            loopEnd = value;
            hasEnd = TRUE;
            continue;
        }

        if (ReadCommentU32(s, "LOOPLENGTH", &value) ||
            ReadCommentU32(s, "LOOP_LENGTH", &value)) {
            loopLength = value;
            hasLength = TRUE;
            continue;
        }
    }

    unsigned int totalSamples = stb_vorbis_stream_length_in_samples(vorbis);

    if (hasStart && hasEnd) {
        info.start = loopStart;
        info.end = loopEnd;
    } else if (hasStart && hasLength && loopLength > 0 && loopStart <= 0xFFFFFFFFu - loopLength) {
        info.start = loopStart;
        info.end = loopStart + loopLength;
    } else if (hasStart && totalSamples > loopStart) {
        // LOOPSTART only: loop from LOOPSTART to EOF.
        info.start = loopStart;
        info.end = totalSamples;
    } else {
        return info;
    }

    if (hasStart && hasEnd && hasLength) {
        unsigned int computed = 0;
        if (info.end >= info.start) {
            computed = info.end - info.start;
        }

        if (computed != loopLength) {
            LogLine(
                "ExternalOGG V22: LOOPLENGTH mismatch path=\"%s\" start=%u end=%u lengthTag=%u computed=%u; using start/end",
                path ? path : "",
                info.start,
                info.end,
                loopLength,
                computed
            );
        }
    }

    if (info.end > totalSamples) {
        LogLine(
            "ExternalOGG V22: LOOPEND exceeds stream length path=\"%s\" end=%u total=%u; clamped",
            path ? path : "",
            info.end,
            totalSamples
        );
        info.end = totalSamples;
    }

    if (info.end <= info.start) {
        LogLine(
            "ExternalOGG V22: invalid loop tags path=\"%s\" start=%u end=%u total=%u; disabled",
            path ? path : "",
            info.start,
            info.end,
            totalSamples
        );
        return EmptyOggLoopInfo();
    }

    info.length = info.end - info.start;
    info.enabled = TRUE;

    LogLine(
        "ExternalOGG V22: loop tags path=\"%s\" start=%u end=%u length=%u total=%u",
        path ? path : "",
        info.start,
        info.end,
        info.length,
        totalSamples
    );

    return info;
}

static unsigned int NormalizeLoopSample(unsigned int sample, const OggLoopInfo *loopInfo)
{
    if (!loopInfo || !loopInfo->enabled || loopInfo->length == 0) return sample;
    if (sample < loopInfo->end) return sample;

    return loopInfo->start + ((sample - loopInfo->start) % loopInfo->length);
}

static short ScaleSample(short sample, float gain)
{
    int v = (int)((float)sample * gain);
    if (v > 32767) v = 32767;
    if (v < -32768) v = -32768;
    return (short)v;
}

static LONG ClampDsbVolume(LONG volume)
{
    if (volume > 0) return 0;
    if (volume < -10000) return -10000;
    return volume;
}

static LONG ClampDsbPan(LONG pan)
{
    if (pan > 10000) return 10000;
    if (pan < -10000) return -10000;
    return pan;
}

static float DsbDb100ToGain(LONG db100)
{
    db100 = ClampDsbVolume(db100);
    if (db100 <= -10000) return 0.0f;
    if (db100 >= 0) return 1.0f;
    return (float)pow(10.0, (double)db100 / 2000.0);
}

static void ApplyVolumeAndPan(short *samples, int sampleCount, int channels)
{
    if (!samples || sampleCount <= 0) return;

    LONG volume = ClampDsbVolume(InterlockedCompareExchange(&g_currentVolume, 0, 0));
    LONG pan = ClampDsbPan(InterlockedCompareExchange(&g_currentPan, 0, 0));
    if (volume == 0 && pan == 0) return;

    float baseGain = DsbDb100ToGain(volume);
    if (channels >= 2) {
        float leftGain = baseGain;
        float rightGain = baseGain;

        // DirectSound pan is represented as attenuation on the opposite channel.
        // DSBPAN_LEFT  (-10000) silences the right channel.
        // DSBPAN_RIGHT (+10000) silences the left channel.
        if (pan < 0) {
            rightGain *= DsbDb100ToGain(pan);
        } else if (pan > 0) {
            leftGain *= DsbDb100ToGain(-pan);
        }

        for (int i = 0; i + 1 < sampleCount; i += 2) {
            samples[i] = ScaleSample(samples[i], leftGain);
            samples[i + 1] = ScaleSample(samples[i + 1], rightGain);
        }
        if ((sampleCount & 1) != 0) {
            samples[sampleCount - 1] = ScaleSample(samples[sampleCount - 1], baseGain);
        }
    } else {
        for (int i = 0; i < sampleCount; ++i) {
            samples[i] = ScaleSample(samples[i], baseGain);
        }
    }
}

static BOOL FillPcmBuffer(
    stb_vorbis *vorbis,
    short *dst,
    int sampleCount,
    int channels,
    BOOL loop,
    const OggLoopInfo *loopInfo,
    unsigned int *decodeSample
) {
    int filled = 0;
    BOOL useTaggedLoop = (loop && loopInfo && loopInfo->enabled);
    unsigned int pos = decodeSample ? *decodeSample : 0;

    while (filled < sampleCount) {
        int remainingShorts = sampleCount - filled;
        int requestFrames = remainingShorts / channels;

        if (requestFrames <= 0) {
            break;
        }

        if (useTaggedLoop) {
            if (pos >= loopInfo->end) {
                int seekOk = stb_vorbis_seek(vorbis, loopInfo->start);
                if (!seekOk) {
                    LogLine(
                        "ExternalOGG V22: loop seek failed start=%u end=%u; falling back to full-file loop",
                        loopInfo->start,
                        loopInfo->end
                    );
                    useTaggedLoop = FALSE;
                } else {
                    pos = loopInfo->start;
                    if (decodeSample) *decodeSample = pos;
                    continue;
                }
            }

            if (useTaggedLoop) {
                unsigned int framesToLoopEnd = loopInfo->end - pos;
                if (framesToLoopEnd < (unsigned int)requestFrames) {
                    requestFrames = (int)framesToLoopEnd;
                }

                if (requestFrames <= 0) {
                    continue;
                }
            }
        }

        int requestShorts = requestFrames * channels;
        int gotFrames = stb_vorbis_get_samples_short_interleaved(
            vorbis,
            channels,
            dst + filled,
            requestShorts
        );
        int gotShorts = gotFrames * channels;

        if (gotShorts > 0) {
            filled += gotShorts;
            pos += (unsigned int)gotFrames;
            if (decodeSample) *decodeSample = pos;
            continue;
        }

        if (loop) {
            unsigned int target = useTaggedLoop ? loopInfo->start : 0;
            int seekOk = useTaggedLoop
                ? stb_vorbis_seek(vorbis, target)
                : stb_vorbis_seek_start(vorbis);

            if (!seekOk) {
                LogLine(
                    "ExternalOGG V22: loop seek failed target=%u tagged=%s",
                    target,
                    MMX3BoolText(useTaggedLoop)
                );

                memset(dst + filled, 0, (sampleCount - filled) * sizeof(short));
                ApplyVolumeAndPan(dst, sampleCount, channels);
                return FALSE;
            }

            pos = target;
            if (decodeSample) *decodeSample = pos;
            continue;
        }

        memset(dst + filled, 0, (sampleCount - filled) * sizeof(short));
        ApplyVolumeAndPan(dst, sampleCount, channels);
        return FALSE;
    }

    ApplyVolumeAndPan(dst, sampleCount, channels);
    return TRUE;
}

static void CloseWaveOutAndBuffers(WAVEHDR *headers, short **buffers, int count)
{
    if (g_waveOut) {
        waveOutReset(g_waveOut);
        for (int i = 0; i < count; ++i) {
            if (headers[i].dwFlags & WHDR_PREPARED) {
                waveOutUnprepareHeader(g_waveOut, &headers[i], sizeof(WAVEHDR));
            }
        }
        waveOutClose(g_waveOut);
        g_waveOut = NULL;
    }
    for (int i = 0; i < count; ++i) {
        if (buffers[i]) {
            GlobalFree(buffers[i]);
            buffers[i] = NULL;
        }
    }
}

static unsigned __stdcall OggThreadMain(void *arg)
{
    OggThreadParam *param = (OggThreadParam *)arg;
    char path[MAX_PATH];
    lstrcpynA(path, param->path, sizeof(path));
    BOOL loop = param->loop;
    int bufferCount = param->bufferCount;
    int bufferMs = param->bufferMs;
    int soundId = param->soundId;
    unsigned int startSample = param->startSample;
    GlobalFree(param);

    int error = 0;
    stb_vorbis *vorbis = stb_vorbis_open_filename(path, &error, NULL);
    if (!vorbis) {
        LogLine("ExternalOGG: stb_vorbis_open failed path=\"%s\" err=%d", path, error);
        return 0;
    }

    stb_vorbis_info info = stb_vorbis_get_info(vorbis);
    int channels = info.channels;
    int rate = info.sample_rate;
    if (channels < 1 || channels > 2 || rate <= 0) {
        LogLine("ExternalOGG: unsupported OGG format path=\"%s\" rate=%d channels=%d", path, rate, channels);
        stb_vorbis_close(vorbis);
        return 0;
    }

    OggLoopInfo loopInfo = ReadOggLoopInfo(vorbis, path);
    if (!loop) {
        loopInfo.enabled = FALSE;
    }

    unsigned int decodeSample = 0;
    if (startSample != 0) {
        unsigned int seekSample = NormalizeLoopSample(startSample, &loopInfo);
        int seekOk = stb_vorbis_seek(vorbis, seekSample);
        if (seekOk) {
            decodeSample = seekSample;
        } else {
            decodeSample = 0;
        }
        LogLine(
            "ExternalOGG V22: resume seek soundId=%d sample=%u actual=%u ok=%d path=\"%s\"",
            soundId,
            startSample,
            decodeSample,
            seekOk,
            path
        );
    }
    InterlockedExchange(&g_currentSampleOffset, (LONG)decodeSample);

    WAVEFORMATEX wf;
    ZeroMemory(&wf, sizeof(wf));
    wf.wFormatTag = WAVE_FORMAT_PCM;
    wf.nChannels = (WORD)channels;
    wf.nSamplesPerSec = rate;
    wf.wBitsPerSample = 16;
    wf.nBlockAlign = (WORD)(channels * 2);
    wf.nAvgBytesPerSec = rate * wf.nBlockAlign;

    WAVEHDR *headers = (WAVEHDR *)GlobalAlloc(GPTR, sizeof(WAVEHDR) * bufferCount);
    short **buffers = (short **)GlobalAlloc(GPTR, sizeof(short *) * bufferCount);
    if (!headers || !buffers) {
        LogLine("ExternalOGG: buffer header allocation failed");
        if (headers) GlobalFree(headers);
        if (buffers) GlobalFree(buffers);
        stb_vorbis_close(vorbis);
        return 0;
    }

    int framesPerBuffer = (rate * bufferMs) / 1000;
    if (framesPerBuffer < 256) framesPerBuffer = 256;
    int samplesPerBuffer = framesPerBuffer * channels;
    int bytesPerBuffer = samplesPerBuffer * sizeof(short);

    MMRESULT mm = waveOutOpen(&g_waveOut, WAVE_MAPPER, &wf, (DWORD_PTR)g_waveEvent, 0, CALLBACK_EVENT);
    if (mm != MMSYSERR_NOERROR) {
        LogLine("ExternalOGG: waveOutOpen failed mm=%u", (unsigned)mm);
        GlobalFree(headers);
        GlobalFree(buffers);
        stb_vorbis_close(vorbis);
        return 0;
    }

    LogLine("ExternalOGG: streaming thread start path=\"%s\" rate=%d channels=%d loop=%s buffers=%d x %dms", path, rate, channels, MMX3BoolText(loop), bufferCount, bufferMs);

    BOOL eofNoLoop = FALSE;
    BOOL eofLogged = FALSE;
    for (int i = 0; i < bufferCount; ++i) {
        buffers[i] = (short *)GlobalAlloc(GPTR, bytesPerBuffer);
        if (!buffers[i]) break;
        eofNoLoop = !FillPcmBuffer(vorbis, buffers[i], samplesPerBuffer, channels, loop, &loopInfo, &decodeSample) || eofNoLoop;
        InterlockedExchange(&g_currentSampleOffset, (LONG)decodeSample);
        headers[i].lpData = (LPSTR)buffers[i];
        headers[i].dwBufferLength = bytesPerBuffer;
        waveOutPrepareHeader(g_waveOut, &headers[i], sizeof(WAVEHDR));
        waveOutWrite(g_waveOut, &headers[i], sizeof(WAVEHDR));
    }

    if (InterlockedCompareExchange(&g_gameSleepPauseActive, 0, 0) != 0) {
        waveOutPause(g_waveOut);
        InterlockedExchange(&g_externalPaused, 1);
        LogLine("ExternalOGG: new stream starts paused because game sleep/pause is active");
    }

    while (InterlockedCompareExchange(&g_stopRequested, 0, 0) == 0) {
        WaitForSingleObject(g_waveEvent, 20);
        FlushPendingFakeStopIfExpired();
        FlushPendingDeactivateStopIfExpired();

        if (InterlockedCompareExchange(&g_outputStopped, 0, 0) != 0) {
            // Persistent DirectSound-like Stop state: waveOutReset has returned
            // all prepared buffers as DONE, but we intentionally do not refill
            // them until the original sound layer calls Play again.
            Sleep(5);
            continue;
        }

        BOOL wroteAny = FALSE;
        for (int i = 0; i < bufferCount; ++i) {
            if ((headers[i].dwFlags & WHDR_DONE) && buffers[i]) {
                if (eofNoLoop) {
                    // Natural end for a non-looping track: keep the waveOut stream alive
                    // by feeding silence. Cleanup happens later through Stop/Release or
                    // a new Play call. This avoids closing waveOut from the playback thread
                    // while WinMM may still have queued callbacks/buffers.
                    memset(buffers[i], 0, samplesPerBuffer * sizeof(short));
                    InterlockedExchange(&g_naturalEnded, 1);
                } else {
                    eofNoLoop = !FillPcmBuffer(vorbis, buffers[i], samplesPerBuffer, channels, loop, &loopInfo, &decodeSample);
                    InterlockedExchange(&g_currentSampleOffset, (LONG)decodeSample);
                    if (eofNoLoop) {
                        InterlockedExchange(&g_naturalEnded, 1);
                        if (!eofLogged) {
                            eofLogged = TRUE;
                            LogLine("ExternalOGG: natural EOF reached path=\"%s\"", path);
                        }
                    }
                }
                waveOutWrite(g_waveOut, &headers[i], sizeof(WAVEHDR));
                wroteAny = TRUE;
            }
        }
        if (!wroteAny && eofNoLoop) {
            Sleep(5);
        }
    }

    CloseWaveOutAndBuffers(headers, buffers, bufferCount);
    GlobalFree(headers);
    GlobalFree(buffers);
    stb_vorbis_close(vorbis);

    LogLine("ExternalOGG: streaming thread end path=\"%s\"", path);
    return 0;
}

static void ForceSlotWasPlayingBeforeDeactivate(int soundId, const char *reason)
{
    if (soundId < 0 || soundId >= (int)kSoundSlotCount) return;
    BYTE *slot = (BYTE *)(uintptr_t)(kSoundSlotBaseAddr + ((DWORD)soundId * kSoundSlotSize));
    __try {
        *(DWORD *)(slot + kSlotWasPlaying) = 1;
        LogLine("ExternalOGG V15: forced slot+0x114 wasPlaying soundId=%d reason=%s",
                soundId, reason ? reason : "unknown");
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        LogLine("ExternalOGG V15: failed to force slot+0x114 wasPlaying soundId=%d", soundId);
    }
}

static void HardStopExternalOggPlayback()
{
    EnsureLock();
    CancelPendingFakeStop("hard stop");
    CancelDelayedGameResume("hard stop");
    HANDLE thread = NULL;
    EnterCriticalSection(&g_lock);
    if (g_waveOut) {
        waveOutReset(g_waveOut);
    }
    InterlockedExchange(&g_stopRequested, 1);
    InterlockedExchange(&g_naturalEnded, 0);
    InterlockedExchange(&g_outputStopped, 0);
    if (g_stopEvent) SetEvent(g_stopEvent);
    thread = g_thread;
    LeaveCriticalSection(&g_lock);

    if (thread) {
        WaitForSingleObject(thread, 1000);
        CloseHandle(thread);
        EnterCriticalSection(&g_lock);
        if (g_thread == thread) g_thread = NULL;
        LeaveCriticalSection(&g_lock);
    }

    InterlockedExchange(&g_externalPaused, 0);
    g_currentSoundId = -1;
    g_currentOggPath[0] = 0;
    InterlockedExchange(&g_currentSampleOffset, 0);
    InterlockedExchange(&g_currentVolume, 0);
    InterlockedExchange(&g_currentPan, 0);
}

static void StopExternalOggOutputForDeactivate(int soundId)
{
    EnsureLock();
    CancelPendingFakeStop("deactivate stop guard expired");
    CancelDelayedGameResume("deactivate stop guard expired");

    EnterCriticalSection(&g_lock);
    if (g_currentSoundId == soundId && g_waveOut) {
        InterlockedExchange(&g_outputStopped, 1);
        InterlockedExchange(&g_externalPaused, 0);
        MMRESULT mm = waveOutReset(g_waveOut);
        LogLine("ExternalOGG V15: delayed deactivate Stop/reset soundId=%d waveOutReset mm=%u", soundId, (unsigned)mm);
    } else {
        LogLine("ExternalOGG V15: delayed deactivate Stop/reset ignored soundId=%d currentSoundId=%d waveOut=%p", soundId, g_currentSoundId, g_waveOut);
    }
    LeaveCriticalSection(&g_lock);
}

static void RequestExternalOggStopNow()
{
    // v21: do not apply an extra DLL-side fade. The original game already
    // drives fade behavior through IDirectSoundBuffer::SetVolume(), which our
    // fake buffer now applies to PCM output. Release/cleanup should therefore
    // stop the waveOut stream directly instead of layering another fade on top.
    EnsureLock();
    EnterCriticalSection(&g_lock);
    if (g_waveOut) {
        MMRESULT mm = waveOutReset(g_waveOut);
        OggTrace("ExternalOGG V21: stop requested waveOutReset mm=%u", (unsigned)mm);
    }
    InterlockedExchange(&g_stopRequested, 1);
    if (g_waveEvent) SetEvent(g_waveEvent);
    LeaveCriticalSection(&g_lock);
}

static BOOL StartExternalOggPlayback(int soundId, const char *path, BOOL loop)
{
    EnsureLock();
    if (!path || !path[0]) return FALSE;

    EnterCriticalSection(&g_lock);
    BOOL sameTrackPlaying = (g_thread != NULL && lstrcmpiA(g_currentOggPath, path) == 0 && g_currentSoundId == soundId && InterlockedCompareExchange(&g_naturalEnded, 0, 0) == 0);
    LeaveCriticalSection(&g_lock);
    if (sameTrackPlaying) {
        g_currentLoop = loop;
        if (CancelPendingDeactivateStopForSound(soundId, "same-track Play before guard expiry")) {
            OggTrace("ExternalOGG V15: transient deactivate Stop/Play pulse swallowed soundId=%d path=\"%s\" loop=%s sample=%ld",
                    soundId, path, MMX3BoolText(loop), InterlockedCompareExchange(&g_currentSampleOffset, 0, 0));
            return TRUE;
        }
        if (InterlockedCompareExchange(&g_outputStopped, 0, 0) != 0) {
            InterlockedExchange(&g_outputStopped, 0);
            InterlockedExchange(&g_externalPaused, 0);
            if (g_waveEvent) SetEvent(g_waveEvent);
            OggTrace("ExternalOGG V15: same-track Play resumes persistent stopped stream soundId=%d path=\"%s\" loop=%s sample=%ld",
                    soundId, path, MMX3BoolText(loop), InterlockedCompareExchange(&g_currentSampleOffset, 0, 0));
        } else {
            OggTrace("ExternalOGG: same-track play suppressed soundId=%d path=\"%s\" loop=%s", soundId, path, MMX3BoolText(loop));
        }
        return TRUE;
    }

    CancelPendingDeactivateStopForSound(soundId, "new Play before guard expiry");

    unsigned int startSample = 0;
    LONG pendingResumeSoundId = InterlockedCompareExchange(&g_deactivateResumePendingSoundId, -1, -1);
    if (pendingResumeSoundId == soundId) {
        startSample = g_resumeSampleOffset[soundId];
        InterlockedExchange(&g_deactivateResumePendingSoundId, -1);
        OggTrace("ExternalOGG V15: applying deactivate resume offset soundId=%d sample=%u", soundId, startSample);
    }

    HardStopExternalOggPlayback();

    OggThreadParam *p = (OggThreadParam *)GlobalAlloc(GPTR, sizeof(OggThreadParam));
    if (!p) return FALSE;
    lstrcpynA(p->path, path, sizeof(p->path));
    p->loop = loop;
    p->bufferCount = g_bufferCount;
    p->bufferMs = g_bufferMs;
    p->soundId = soundId;
    p->startSample = startSample;

    InterlockedExchange(&g_stopRequested, 0);
    InterlockedExchange(&g_naturalEnded, 0);
    InterlockedExchange(&g_externalPaused, 0);
    InterlockedExchange(&g_outputStopped, 0);
    if (g_stopEvent) ResetEvent(g_stopEvent);

    uintptr_t th = _beginthreadex(NULL, 0, OggThreadMain, p, 0, NULL);
    if (!th) {
        GlobalFree(p);
        return FALSE;
    }

    EnterCriticalSection(&g_lock);
    g_thread = (HANDLE)th;
    g_currentSoundId = soundId;
    g_currentLoop = loop;
    lstrcpynA(g_currentOggPath, path, sizeof(g_currentOggPath));
    LeaveCriticalSection(&g_lock);

    return TRUE;
}

static void PauseExternalOggPlayback()
{
    EnsureLock();
    EnterCriticalSection(&g_lock);
    if (g_waveOut && InterlockedCompareExchange(&g_externalPaused, 1, 0) == 0) {
        MMRESULT mm = waveOutPause(g_waveOut);
        LogLine("ExternalOGG: waveOutPause by game sleep/pause mm=%u", (unsigned)mm);
    }
    LeaveCriticalSection(&g_lock);
}

static void ResumeExternalOggPlayback()
{
    EnsureLock();
    EnterCriticalSection(&g_lock);
    if (g_waveOut && InterlockedCompareExchange(&g_externalPaused, 0, 1) == 1) {
        MMRESULT mm = waveOutRestart(g_waveOut);
        LogLine("ExternalOGG: waveOutRestart by game active mm=%u", (unsigned)mm);
    }
    LeaveCriticalSection(&g_lock);
}


static void ResumeExternalOggSlotsAfterActivate()
{
    for (int soundId = 0; soundId < (int)kSoundSlotCount; ++soundId) {
        if (!g_slots[soundId].active || !g_slots[soundId].hasOgg) {
            continue;
        }

        BYTE *slot = SlotPtr(soundId);
        if (!slot) {
            continue;
        }

        DWORD loaded = 0;
        DWORD wasPlaying = 0;
        DWORD playParam = 0;
        DWORD streamingFlag = 0;
        FakeDirectSoundBuffer *fake = NULL;

        __try {
            loaded = *(DWORD *)(slot + kSlotLoaded);
            wasPlaying = *(DWORD *)(slot + kSlotWasPlaying);
            playParam = *(DWORD *)(slot + kSlotPlayParam);
            streamingFlag = *(DWORD *)(slot + kSlotStreamingFlag);
            fake = *(FakeDirectSoundBuffer **)(slot + kSlotBuffers);
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            OggTrace("ExternalOGG V15: activate resume scan exception soundId=%d", soundId);
            continue;
        }

        // FUN_004E0E30 resumes only when:
        //   loaded && wasPlaying && (slot+0x12C loop/playParam || slot+0x170 streamingFlag)
        // Our fake bridge deliberately keeps streamingFlag=0 to avoid the native
        // stream cleanup path and a NULL stream pointer.  Therefore non-loop OGG
        // tracks such as X3_03 need this explicit resume path.
        if (loaded != 0 && wasPlaying != 0 && playParam == 0 && streamingFlag == 0 && fake != NULL) {
            OggTrace("ExternalOGG V15: manual activate resume soundId=%d playParam=%lu streamingFlag=%lu ogg=\"%s\"",
                    soundId, (unsigned long)playParam, (unsigned long)streamingFlag, g_slots[soundId].oggPath);
            FakeDSB_Play(fake, 0, 0, 0);
            __try {
                *(DWORD *)(slot + kSlotWasPlaying) = 0;
            } __except(EXCEPTION_EXECUTE_HANDLER) {
            }
        }
    }
}

static void __cdecl HookActivateResumeAll()
{
    void *caller = _ReturnAddress();
    OggTrace("ExternalOGG V15: Sound_ActivateResumeAll_004E0E30 begin");
    if (g_realActivateResumeAll) {
        g_realActivateResumeAll();
    }
    // v12: no manual replay here. The fake slot is marked as a streaming slot
    // with safe dummy stream metadata, so the original 004E0E30 condition can
    // naturally call Sound_PlaySlot and reach FakeDSB_Play with callerRet=004E0CE0.
    OggTrace("ExternalOGG V15: Sound_ActivateResumeAll_004E0E30 end");
    OggTrace("ExternalOGG TRACE: 004E0E30 leave callerRet=%p", caller);
}

static void ClearFakeOggBookkeepingOnly(int soundId)
{
    if (soundId < 0 || soundId >= (int)kSoundSlotCount) return;

    // Only clear our own bookkeeping here. Do not touch the game's slot unless
    // g_slots[soundId].active proves we previously installed a fake OGG slot.
    if (g_slots[soundId].active) {
        ClearExternalSlot(soundId);
    }

    g_slots[soundId].active = FALSE;
    g_slots[soundId].hasOgg = FALSE;
    g_slots[soundId].nativePath[0] = 0;
    g_slots[soundId].oggPath[0] = 0;
    g_resumeSampleOffset[soundId] = 0;
}


static BOOL ReinstallRegisterStreamingWaveHook()
{
    if (!g_registerTarget || g_registerStolen < 5 || g_registerStolen > 16) {
        return FALSE;
    }

    BYTE patch[16];
    memset(patch, 0x90, sizeof(patch));
    patch[0] = 0xE9;
    *(DWORD *)(patch + 1) = (DWORD)((BYTE *)HookRegisterStreamingWave - (g_registerTarget + 5));

    if (!PatchMemory(g_registerTarget, patch, g_registerStolen)) {
        LogLine("ExternalOGG: native fallback failed to reinstall register hook");
        return FALSE;
    }
    return TRUE;
}

static int CallOriginalRegisterStreamingWaveNative(LPCSTR path, int soundId)
{
    // If the hook is not installed yet, fall back to the gateway if available.
    if (!g_registerTarget || g_registerStolen < 5 || g_registerStolen > 16) {
        return g_realRegisterStreamingWave ? g_realRegisterStreamingWave(path, soundId) : 0;
    }

    if (InterlockedIncrement(&g_registerNativeCallDepth) != 1) {
        InterlockedDecrement(&g_registerNativeCallDepth);
        LogLine("ExternalOGG: recursive native register fallback blocked path=\"%s\" soundId=%d", path ? path : "", soundId);
        return 0;
    }

    int result = 0;
    BOOL restored = PatchMemory(g_registerTarget, g_registerOriginalBytes, g_registerStolen);
    if (!restored) {
        LogLine("ExternalOGG: native fallback failed to restore original register bytes path=\"%s\" soundId=%d", path ? path : "", soundId);
    } else {
        FlushInstructionCache(GetCurrentProcess(), g_registerTarget, g_registerStolen);
        RegisterStreamingWaveFn fn = (RegisterStreamingWaveFn)g_registerTarget;
        result = fn(path, soundId);
    }

    ReinstallRegisterStreamingWaveHook();
    InterlockedDecrement(&g_registerNativeCallDepth);
    return result;
}

static int __cdecl HookRegisterStreamingWave(LPCSTR path, int soundId)
{
    if (!g_enabled || !g_replaceNativeStream || !IsBgmSePath(path) || soundId < 0 || soundId >= (int)kSoundSlotCount) {
        return CallOriginalRegisterStreamingWaveNative(path, soundId);
    }

    // v17 fallback hardening: resolve and validate the external OGG before
    // touching any DirectSound slot state. This allows partial BGM replacement:
    // missing BGM_EXT\X3_xx.ogg files fall back to the original native .SE/.WAV
    // path with no half-installed fake buffer, dummy stream, or streaming flag.
    char oggPath[MAX_PATH];
    oggPath[0] = 0;
    if (!BuildExternalOggPath(path, oggPath, sizeof(oggPath))) {
        ClearFakeOggBookkeepingOnly(soundId);
        LogLine("ExternalOGG: unable to resolve external OGG path for native=\"%s\" soundId=%d -> fallback native WAV", path ? path : "", soundId);
        return CallOriginalRegisterStreamingWaveNative(path, soundId);
    }

    if (!FileExistsA2(oggPath)) {
        ClearFakeOggBookkeepingOnly(soundId);
        LogLine("ExternalOGG: external OGG missing native=\"%s\" ogg=\"%s\" soundId=%d -> fallback native WAV", path ? path : "", oggPath, soundId);
        return CallOriginalRegisterStreamingWaveNative(path, soundId);
    }

    void *fakeBuffer = NULL;
    if (!CreateFakeDirectSoundBuffer(soundId, oggPath, &fakeBuffer)) {
        ClearFakeOggBookkeepingOnly(soundId);
        LogLine("ExternalOGG: fake buffer creation failed soundId=%d native=\"%s\" ogg=\"%s\" -> fallback native WAV", soundId, path ? path : "", oggPath);
        return CallOriginalRegisterStreamingWaveNative(path, soundId);
    }

    if (!MarkExternalSlotRegistered(soundId, path, fakeBuffer)) {
        LogLine("ExternalOGG: fake register failed to mark slot soundId=%d native=\"%s\" ogg=\"%s\" -> fallback native WAV", soundId, path ? path : "", oggPath);
        // Release the fake buffer manually because ownership was not handed to the game slot.
        void **vt = *(void ***)fakeBuffer;
        typedef ULONG (__stdcall *ReleaseFn)(void *self);
        ((ReleaseFn)vt[2])(fakeBuffer);
        ClearFakeOggBookkeepingOnly(soundId);
        return CallOriginalRegisterStreamingWaveNative(path, soundId);
    }

    g_slots[soundId].active = TRUE;
    g_slots[soundId].hasOgg = TRUE;
    lstrcpynA(g_slots[soundId].nativePath, path, sizeof(g_slots[soundId].nativePath));
    lstrcpynA(g_slots[soundId].oggPath, oggPath, sizeof(g_slots[soundId].oggPath));

    LogLine("ExternalOGG: fake bridge register soundId=%d path=\"%s\" ogg=\"%s\" -> success with fake DS buffer=%p", soundId, path, oggPath, fakeBuffer);
    return 1;
}

static int __cdecl HookPlaySlot(int soundId, int loopFlag)
{
    if (g_enabled && g_replaceNativeStream && soundId >= 0 && soundId < (int)kSoundSlotCount && g_slots[soundId].active && g_slots[soundId].hasOgg) {
        BOOL loop = loopFlag ? TRUE : FALSE;
        LogLine("ExternalOGG: pure Sound_Play soundId=%d originalLoop=%d -> start ogg=\"%s\" loop=%s", soundId, loopFlag, g_slots[soundId].oggPath, MMX3BoolText(loop));
        return StartExternalOggPlayback(soundId, g_slots[soundId].oggPath, loop) ? 0 : -1;
    }
    return g_realPlaySlot ? g_realPlaySlot(soundId, loopFlag) : -1;
}

static void __cdecl HookReleaseSlot(int soundId)
{
    if (g_enabled && g_replaceNativeStream && soundId >= 0 && soundId < (int)kSoundSlotCount && g_slots[soundId].active) {
        LogLine("ExternalOGG: pure Sound_Release soundId=%d ogg=\"%s\" -> fade external and release dummy native buffer", soundId, g_slots[soundId].oggPath);
        if (g_currentSoundId == soundId) {
            RequestExternalOggStopNow();
        }

        // Let the original release path stop/release slot->buffers[0]. In v2 that
        // buffer is our dummy DirectSoundBuffer, so the original code can clean it
        // up safely and keep the game sound table consistent.
        if (g_realReleaseSlot) {
            g_realReleaseSlot(soundId);
        } else {
            ClearExternalSlot(soundId);
        }

        g_slots[soundId].active = FALSE;
        g_slots[soundId].hasOgg = FALSE;
        g_slots[soundId].nativePath[0] = 0;
        g_slots[soundId].oggPath[0] = 0;
        return;
    }
    if (g_realReleaseSlot) g_realReleaseSlot(soundId);
}



static void ResumeBlockedPlayAfterUserSleepHold()
{
    LONG blocked = InterlockedExchange(&g_sleepBlockedPlaySoundId, -1);
    if (blocked < 0 || blocked >= (LONG)kSoundSlotCount) {
        LogLine("ExternalOGG V15: user sleep hold OFF blockedPlaySoundId=%ld", blocked);
        return;
    }

    BYTE *slot = SlotPtr((int)blocked);
    FakeDirectSoundBuffer *fake = NULL;
    __try {
        if (slot) fake = *(FakeDirectSoundBuffer **)(slot + kSlotBuffers);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        fake = NULL;
    }

    const char *path = NULL;
    BOOL loop = g_sleepBlockedPlayLoop;
    if (fake) {
        path = fake->oggPath;
    } else if (g_slots[blocked].hasOgg) {
        path = g_slots[blocked].oggPath;
    }

    LogLine("ExternalOGG V15: user sleep hold OFF blockedPlaySoundId=%ld fake=%p loop=%s path=\"%s\"",
            blocked, fake, MMX3BoolText(loop), path ? path : "");

    if (path && path[0]) {
        if (StartExternalOggPlayback((int)blocked, path, loop)) {
            if (fake) InterlockedExchange(&fake->playing, 1);
            LogLine("ExternalOGG V15: replayed blocked Play after user sleep hold soundId=%ld loop=%s", blocked, MMX3BoolText(loop));
        } else {
            LogLine("ExternalOGG V15: failed to replay blocked Play after user sleep hold soundId=%ld", blocked);
        }
    }
}

static void TraceWindowState(const char *tag, int *self)
{
    HWND hwnd = NULL;
    if (self) hwnd = (HWND)self[2];
    HWND fg = GetForegroundWindow();
    char title[256];
    title[0] = 0;
    if (hwnd) {
        GetWindowTextA(hwnd, title, sizeof(title));
    }
    OggTrace("ExternalOGG TRACE: %s hwnd=%p fg=%p sameFg=%d iconic=%d visible=%d title=\"%s\"",
            tag ? tag : "window", hwnd, fg, (hwnd && fg == hwnd) ? 1 : 0,
            hwnd ? (int)IsIconic(hwnd) : -1,
            hwnd ? (int)IsWindowVisible(hwnd) : -1,
            title);
}

// v15: HookAppAudioActiveState removed intentionally.

static void __fastcall HookGameSleepPauseState(int *self, void *edx, int param2, int param3)
{
    (void)edx;
    void *caller = _ReturnAddress();
    int oldSleep = self ? self[8] : 0;
    int oldPause = self ? self[9] : 0;
    BOOL preSleepOn = (param3 == 0 && param2 != 0);

    OggTrace("ExternalOGG TRACE: 004DE0D0 enter callerRet=%p this=%p param2=%d param3=%d old=(%d,%d)", caller, self, param2, param3, oldSleep, oldPause);
    TraceWindowState("004DE0D0 before", self);

    // V15: Sleep ON must mute before entering the original function. The original
    // 004DE0D0 may spend a long time in display/sound transition before it returns;
    // muting only after the original returns is audibly too late.
    if (preSleepOn) {
        if (InterlockedExchange(&g_userSleepHold, 1) == 0) {
            InterlockedExchange(&g_sleepBlockedPlaySoundId, -1);
            g_sleepBlockedPlayLoop = FALSE;
            CancelPendingFakeStop("user sleep pre-hold ON");
            CancelDelayedGameResume("user sleep pre-hold ON");
            LONG sid = g_currentSoundId;
            if (sid >= 0) {
                LogLine("ExternalOGG V15: PRE user sleep hold ON -> mute gate soundId=%ld", sid);
                StopExternalOggOutputForDeactivate((int)sid);
            } else {
                LogLine("ExternalOGG V15: PRE user sleep hold ON -> no current OGG");
            }
        } else {
            LogLine("ExternalOGG V15: PRE user sleep hold already ON");
        }
    }

    if (g_realSleepPauseState) {
        g_realSleepPauseState(self, param2, param3);
    }

    int newSleep = self ? self[8] : 0;
    int newPause = self ? self[9] : 0;
    BOOL shouldPause = (newSleep != 0 || newPause != 0);
    InterlockedExchange(&g_gameSleepPauseActive, shouldPause ? 1 : 0);

    TraceWindowState("004DE0D0 after", self);
    OggTrace("ExternalOGG: game sleep/pause callerRet=%p old=(%d,%d) new=(%d,%d) param2=%d param3=%d -> %s",
            caller, oldSleep, oldPause, newSleep, newPause, param2, param3,
            shouldPause ? "sleep-marker user hold ON by v15" : "resume-marker user hold OFF by v15");

    if (shouldPause) {
        if (!preSleepOn && InterlockedExchange(&g_userSleepHold, 1) == 0) {
            InterlockedExchange(&g_sleepBlockedPlaySoundId, -1);
            g_sleepBlockedPlayLoop = FALSE;
            CancelPendingFakeStop("user sleep hold ON");
            CancelDelayedGameResume("user sleep hold ON");
            LONG sid = g_currentSoundId;
            if (sid >= 0) {
                LogLine("ExternalOGG V15: POST user sleep hold ON -> mute gate soundId=%ld", sid);
                StopExternalOggOutputForDeactivate((int)sid);
            } else {
                LogLine("ExternalOGG V15: POST user sleep hold ON -> no current OGG");
            }
        } else if (preSleepOn) {
            LogLine("ExternalOGG V15: user sleep hold already applied before original");
        }
    } else {
        if (InterlockedExchange(&g_userSleepHold, 0) != 0) {
            ResumeBlockedPlayAfterUserSleepHold();
        } else {
            LONG blocked = InterlockedCompareExchange(&g_sleepBlockedPlaySoundId, -1, -1);
            if (blocked >= 0) {
                ResumeBlockedPlayAfterUserSleepHold();
            }
        }
        if (InterlockedCompareExchange(&g_pendingFakeStopSuspendedBySleep, 0, 0) != 0) {
            CancelPendingFakeStop("game active marker after user sleep hold OFF");
        }
    }
}

static SIZE_T DecodeInstructionLength(const BYTE *p)
{
    BYTE b = p[0];
    if (b == 0x55 || b == 0x56 || b == 0x57 || b == 0x53 || b == 0x52 || b == 0x51 || b == 0x50) return 1;
    if (b == 0x8B || b == 0x89 || b == 0x8D || b == 0x83 || b == 0xC7) return 3;
    if (b == 0x81) return 6;
    if (b == 0x68) return 5;
    if (b == 0x6A) return 2;
    if (b == 0xE8 || b == 0xE9) return 5;
    if (b == 0x83 && (p[1] & 0xC0) == 0xC0) return 3;
    if (b == 0x33 || b == 0x85 || b == 0x3B) return 2;
    if (b == 0xA1 || b == 0xA3) return 5;
    if (b == 0x8A || b == 0x88) return 3;
    if (b == 0xC3) return 1;
    return 1;
}

static BOOL InstallInlineHook(BYTE *target, void *hook, void **trampolineOut, const char *name)
{
    SIZE_T stolen = 0;
    while (stolen < 5) {
        stolen += DecodeInstructionLength(target + stolen);
        if (stolen > 16) break;
    }
    if (stolen < 5 || stolen > 16) {
        LogLine("ExternalOGG: inline hook %s failed: invalid stolen size=%lu", name, (unsigned long)stolen);
        return FALSE;
    }

    if (strcmp(name, "Sound_RegisterStreamingWaveFile_004E14A0") == 0) {
        g_registerTarget = target;
        g_registerStolen = stolen;
        memcpy(g_registerOriginalBytes, target, stolen);
    }

    BYTE *gate = (BYTE *)VirtualAlloc(NULL, stolen + 5, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!gate) {
        LogLine("ExternalOGG: inline hook %s failed: VirtualAlloc", name);
        return FALSE;
    }
    memcpy(gate, target, stolen);
    gate[stolen] = 0xE9;
    *(DWORD *)(gate + stolen + 1) = (DWORD)((target + stolen) - (gate + stolen + 5));

    BYTE patch[16];
    memset(patch, 0x90, sizeof(patch));
    patch[0] = 0xE9;
    *(DWORD *)(patch + 1) = (DWORD)((BYTE *)hook - (target + 5));

    if (!PatchMemory(target, patch, stolen)) {
        LogLine("ExternalOGG: inline hook %s failed: PatchMemory", name);
        return FALSE;
    }
    *trampolineOut = gate;
    LogLine("ExternalOGG: inline hook %s installed target=%p gate=%p stolen=%lu", name, target, gate, (unsigned long)stolen);
    return TRUE;
}

void InstallExternalOggBgmHooks(HMODULE exe)
{
    EnsureLock();
    EnsureAudioDefaults();

    g_enabled = GetPortableConfigBool("Audio", "ExternalOggBgm", TRUE);
    g_replaceNativeStream = GetPortableConfigBool("Audio", "ExternalOggReplaceNativeStream", TRUE);
    g_pauseWithGameSleep = GetPortableConfigBool("Audio", "ExternalOggPauseWithGameSleep", TRUE);
    GetPrivateProfileStringA("Audio", "ExternalBgmPath", "BGM_EXT", g_externalBgmPath, sizeof(g_externalBgmPath), g_configPath);
    g_bufferCount = GetIniInt("Audio", "ExternalOggBufferCount", 3);
    g_bufferMs = GetIniInt("Audio", "ExternalOggBufferMs", 15);
    if (g_bufferCount < 2) g_bufferCount = 2;
    if (g_bufferCount > 8) g_bufferCount = 8;
    if (g_bufferMs < 20) g_bufferMs = 20;
    if (g_bufferMs > 250) g_bufferMs = 250;

    LogLine("ExternalOGG config: enabled=%s path=%s bufferCount=%d bufferMs=%d pauseWithGameSleep=%s replaceNativeStream=%s debugLog=%s mode=fake-ds-buffer-bridge-v22-loop-tags", MMX3BoolText(g_enabled), g_externalBgmPath, g_bufferCount, g_bufferMs, MMX3BoolText(g_pauseWithGameSleep), MMX3BoolText(g_replaceNativeStream), MMX3BoolText(g_verboseLog));

    if (!g_enabled || !g_replaceNativeStream) {
        LogLine("ExternalOGG: fake-buffer bridge disabled");
        return;
    }

    BYTE *base = (BYTE *)exe;
    InstallInlineHook(base + kFnRegisterStreamingWave, (void *)HookRegisterStreamingWave, (void **)&g_realRegisterStreamingWave, "Sound_RegisterStreamingWaveFile_004E14A0");
    InstallInlineHook(base + kFnActivateResumeAll, (void *)HookActivateResumeAll, (void **)&g_realActivateResumeAll, "Sound_ActivateResumeAll_004E0E30");
    // v16: do not hook 00403AC0; previous diagnostic hook proved this trampoline was unsafe.
    // Sound_PlaySlot and Sound_ReleaseSlot are intentionally not hooked in the
    // fake-buffer bridge. The original game calls our fake IDirectSoundBuffer
    // vtable, so Play/Stop/Release semantics are synchronized at the buffer level.
    if (g_pauseWithGameSleep) {
        InstallInlineHook(base + kFnSleepPauseState, (void *)HookGameSleepPauseState, (void **)&g_realSleepPauseState, "Game_SetSleepPauseState_004DE0D0");
    }
}
