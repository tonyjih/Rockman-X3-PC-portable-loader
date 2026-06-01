#include "mmx3_common.h"

#include <windows.h>
#include <mmsystem.h>
#include <stdint.h>
#include <intrin.h>
#include <process.h>
#include <string.h>

#pragma intrinsic(_ReturnAddress)

#ifndef MMX3_ENABLE_TIMING_LOG
#define MMX3_ENABLE_TIMING_LOG MMX3_ENABLE_LOG
#endif

// ============================================================
// Timer experiment mode
// ============================================================

#define MMX3_TIMER_MODE_ORIGINAL       0
#define MMX3_TIMER_MODE_PATCH_16MS     1
#define MMX3_TIMER_MODE_FRACTIONAL_60  2

#ifndef MMX3_TIMER_MODE
#define MMX3_TIMER_MODE MMX3_TIMER_MODE_FRACTIONAL_60
#endif

#define MMX3_ENABLE_16MS_TIMER_EXPERIMENT        (MMX3_TIMER_MODE == MMX3_TIMER_MODE_PATCH_16MS)
#define MMX3_ENABLE_FRACTIONAL_60_EXPERIMENT     (MMX3_TIMER_MODE == MMX3_TIMER_MODE_FRACTIONAL_60)

// ============================================================
// MMX3 known addresses
// ============================================================

#define MMX3_ADDR_MAIN_TIMER_THREAD        ((BYTE *)0x004DEB10)
#define MMX3_ADDR_MAIN_TIMER_PUSH_MS       ((BYTE *)0x00402354)
#define MMX3_ADDR_MAIN_TIMER_CALLBACK      ((void *)0x00403460)

#define MMX3_MAIN_TIMER_ORIGINAL_MS        17
#define MMX3_MAIN_TIMER_ORIGINAL_SLEEP_MS  1

// Original 004DEB10 prologue observed in US build:
//
// 004DEB10 53              push ebx
// 004DEB11 56              push esi
// 004DEB12 8B 74 24 0C     mov esi,[esp+0Ch]
// 004DEB16 57              push edi
// 004DEB17 55              push ebp
//
static const BYTE kMainTimerThreadPrologue[] = {
    0x53, 0x56, 0x8B, 0x74, 0x24, 0x0C, 0x57, 0x55
};

// ============================================================
// Original API pointers
// ============================================================

typedef VOID  (WINAPI *PFN_Sleep)(DWORD dwMilliseconds);
typedef DWORD (WINAPI *PFN_SleepEx)(DWORD dwMilliseconds, BOOL bAlertable);
typedef DWORD (WINAPI *PFN_GetTickCount)(void);
typedef DWORD (WINAPI *PFN_timeGetTime)(void);
typedef BOOL  (WINAPI *PFN_QueryPerformanceCounter)(LARGE_INTEGER *lpPerformanceCount);
typedef BOOL  (WINAPI *PFN_QueryPerformanceFrequency)(LARGE_INTEGER *lpFrequency);

static PFN_Sleep g_realSleep = NULL;
static PFN_SleepEx g_realSleepEx = NULL;
static PFN_GetTickCount g_realGetTickCount = NULL;
static PFN_timeGetTime g_realTimeGetTime = NULL;
static PFN_QueryPerformanceCounter g_realQueryPerformanceCounter = NULL;
static PFN_QueryPerformanceFrequency g_realQueryPerformanceFrequency = NULL;

// ============================================================
// Timing log counters / caller tracking
// ============================================================

#if MMX3_ENABLE_TIMING_LOG

static LONG g_sleepLogCount = 0;
static LONG g_sleepExLogCount = 0;
static LONG g_getTickLogCount = 0;
static LONG g_timeGetTimeLogCount = 0;
static LONG g_qpcLogCount = 0;
static LONG g_qpfLogCount = 0;

static DWORD g_lastGetTickCountValue = 0;
static DWORD g_lastTimeGetTimeValue = 0;
static LARGE_INTEGER g_lastQpcValue = { 0 };
static LARGE_INTEGER g_qpcFrequency = { 0 };

struct TimingCallerSlot
{
    void *caller;
    DWORD lastValue;
    DWORD count;
};

#define MMX3_TIMING_CALLER_SLOTS 64

static TimingCallerSlot g_timeGetTimeCallers[MMX3_TIMING_CALLER_SLOTS];

static void InitQpcFrequency()
{
    if (g_qpcFrequency.QuadPart == 0 && g_realQueryPerformanceFrequency) {
        g_realQueryPerformanceFrequency(&g_qpcFrequency);
    }
}

static const char *GetTimingCallerName(void *caller)
{
    if (caller == (void *)0x004DEB37) {
        return "MainTimer.base";
    }

    if (caller == (void *)0x004DEB4A) {
        return "MainTimer.wait";
    }

    if (caller == (void *)0x004DE1BA) {
        return "WindowOrMessageTimer";
    }

    if (caller == (void *)0x004DE2B0) {
        return "TimerInit";
    }

    if (caller == (void *)0x004DE122) {
        return "TimerResetOrSet";
    }

    return "unknown";
}

static DWORD UpdateCallerDelta(
    TimingCallerSlot *slots,
    DWORD slotCount,
    void *caller,
    DWORD value,
    DWORD *outCount)
{
    if (outCount) {
        *outCount = 0;
    }

    if (!slots || slotCount == 0) {
        return 0;
    }

    DWORD freeIndex = slotCount;

    for (DWORD i = 0; i < slotCount; i++) {
        if (slots[i].caller == caller) {
            DWORD delta = 0;

            if (slots[i].lastValue != 0) {
                delta = value - slots[i].lastValue;
            }

            slots[i].lastValue = value;
            slots[i].count++;

            if (outCount) {
                *outCount = slots[i].count;
            }

            return delta;
        }

        if (slots[i].caller == NULL && freeIndex == slotCount) {
            freeIndex = i;
        }
    }

    if (freeIndex < slotCount) {
        slots[freeIndex].caller = caller;
        slots[freeIndex].lastValue = value;
        slots[freeIndex].count = 1;

        if (outCount) {
            *outCount = 1;
        }
    }

    return 0;
}

#endif

// ============================================================
// MMX3 timer object layout
// ============================================================
//
// Based on decompiled 004DEB10 / 004DEA40:
//
// param[0] = stop flag / pause gate
// param[1] = callback busy
// param[2] = running
// param[5] = thread alive
// param[6] = callback
// param[7] = callback user / owner
// param[8] = interval ms
// param[9] = sleep ms
//
// Keep this as 32-bit only. This proxy targets the original x86 game.
//

struct Mmx3TimerObject;

typedef void (__cdecl *MMX3_TIMER_CALLBACK)(Mmx3TimerObject *timer);

struct Mmx3TimerObject
{
    volatile LONG stopFlag;        // +00 param[0]
    volatile LONG callbackBusy;    // +04 param[1]
    volatile LONG running;         // +08 param[2]
    LONG unknown0c;                // +0C param[3]
    LONG unknown10;                // +10 param[4]
    volatile LONG threadAlive;     // +14 param[5]
    MMX3_TIMER_CALLBACK callback;  // +18 param[6]
    void *callbackUser;            // +1C param[7]
    LONG intervalMs;               // +20 param[8]
    LONG sleepMs;                  // +24 param[9]
};

static bool IsMainTimerObject(Mmx3TimerObject *timer)
{
    if (!timer) {
        return false;
    }

    if ((void *)timer->callback != MMX3_ADDR_MAIN_TIMER_CALLBACK) {
        return false;
    }

    if (timer->intervalMs != MMX3_MAIN_TIMER_ORIGINAL_MS) {
        return false;
    }

    if (timer->sleepMs != MMX3_MAIN_TIMER_ORIGINAL_SLEEP_MS) {
        return false;
    }

    return true;
}

// ============================================================
// Original-like sleep / time helpers
// ============================================================

static DWORD Mmx3RealTimeGetTime()
{
    if (g_realTimeGetTime) {
        return g_realTimeGetTime();
    }

    return timeGetTime();
}

static void Mmx3RealSleepEx(DWORD ms, BOOL alertable)
{
    if (g_realSleepEx) {
        g_realSleepEx(ms, alertable);
        return;
    }

    SleepEx(ms, alertable);
}

// ============================================================
// Hooked APIs
// ============================================================

static VOID WINAPI HookSleep(DWORD dwMilliseconds)
{
#if MMX3_ENABLE_TIMING_LOG
    void *caller = _ReturnAddress();
    LONG n = InterlockedIncrement(&g_sleepLogCount);

    if (n <= 256) {
        LogLine(
            "Timing Sleep #%ld caller=%p ms=%lu",
            (long)n,
            caller,
            (unsigned long)dwMilliseconds);
    }
#endif

    if (g_realSleep) {
        g_realSleep(dwMilliseconds);
    }
}

static DWORD WINAPI HookSleepEx(DWORD dwMilliseconds, BOOL bAlertable)
{
#if MMX3_ENABLE_TIMING_LOG
    void *caller = _ReturnAddress();
    LONG n = InterlockedIncrement(&g_sleepExLogCount);

    if (n <= 256) {
        LogLine(
            "Timing SleepEx #%ld caller=%p ms=%lu alertable=%d",
            (long)n,
            caller,
            (unsigned long)dwMilliseconds,
            (int)bAlertable);
    }
#endif

    if (g_realSleepEx) {
        return g_realSleepEx(dwMilliseconds, bAlertable);
    }

    return 0;
}

static DWORD WINAPI HookGetTickCount(void)
{
    DWORD value = 0;

    if (g_realGetTickCount) {
        value = g_realGetTickCount();
    }

#if MMX3_ENABLE_TIMING_LOG
    void *caller = _ReturnAddress();
    LONG n = InterlockedIncrement(&g_getTickLogCount);

    if (n <= 512) {
        DWORD globalDelta = 0;

        if (g_lastGetTickCountValue != 0) {
            globalDelta = value - g_lastGetTickCountValue;
        }

        g_lastGetTickCountValue = value;

        LogLine(
            "Timing GetTickCount #%ld caller=%p value=%lu globalDelta=%lu",
            (long)n,
            caller,
            (unsigned long)value,
            (unsigned long)globalDelta);
    }
#endif

    return value;
}

static DWORD WINAPI HookTimeGetTime(void)
{
    DWORD value = 0;

    if (g_realTimeGetTime) {
        value = g_realTimeGetTime();
    }

#if (0)
    void *caller = _ReturnAddress();
    LONG n = InterlockedIncrement(&g_timeGetTimeLogCount);

    if (n <= 1024) {
        DWORD globalDelta = 0;

        if (g_lastTimeGetTimeValue != 0) {
            globalDelta = value - g_lastTimeGetTimeValue;
        }

        g_lastTimeGetTimeValue = value;

        DWORD callerCount = 0;
        DWORD callerDelta = UpdateCallerDelta(
            g_timeGetTimeCallers,
            MMX3_TIMING_CALLER_SLOTS,
            caller,
            value,
            &callerCount);

        LogLine(
            "Timing timeGetTime #%ld caller=%p callerName=%s callerCount=%lu value=%lu globalDelta=%lu callerDelta=%lu",
            (long)n,
            caller,
            GetTimingCallerName(caller),
            (unsigned long)callerCount,
            (unsigned long)value,
            (unsigned long)globalDelta,
            (unsigned long)callerDelta);
    }
#endif

    return value;
}

static BOOL WINAPI HookQueryPerformanceFrequency(LARGE_INTEGER *lpFrequency)
{
    BOOL ok = FALSE;

    if (g_realQueryPerformanceFrequency) {
        ok = g_realQueryPerformanceFrequency(lpFrequency);
    }

#if MMX3_ENABLE_TIMING_LOG
    void *caller = _ReturnAddress();
    LONG n = InterlockedIncrement(&g_qpfLogCount);

    if (n <= 16 && ok && lpFrequency) {
        LogLine(
            "Timing QPF #%ld caller=%p frequency=%I64d",
            (long)n,
            caller,
            lpFrequency->QuadPart);
    }
#endif

    return ok;
}

static BOOL WINAPI HookQueryPerformanceCounter(LARGE_INTEGER *lpPerformanceCount)
{
    BOOL ok = FALSE;

    if (g_realQueryPerformanceCounter) {
        ok = g_realQueryPerformanceCounter(lpPerformanceCount);
    }

#if MMX3_ENABLE_TIMING_LOG
    void *caller = _ReturnAddress();
    LONG n = InterlockedIncrement(&g_qpcLogCount);

    if (n <= 512 && ok && lpPerformanceCount) {
        InitQpcFrequency();

        double globalDeltaMs = 0.0;

        if (g_lastQpcValue.QuadPart != 0 && g_qpcFrequency.QuadPart != 0) {
            LONGLONG delta = lpPerformanceCount->QuadPart - g_lastQpcValue.QuadPart;
            globalDeltaMs = (double)delta * 1000.0 / (double)g_qpcFrequency.QuadPart;
        }

        g_lastQpcValue = *lpPerformanceCount;

        LogLine(
            "Timing QPC #%ld caller=%p value=%I64d globalDeltaMs=%.4f",
            (long)n,
            caller,
            lpPerformanceCount->QuadPart,
            globalDeltaMs);
    }
#endif

    return ok;
}

// ============================================================
// Fractional 60fps main timer runtime
// ============================================================

static DWORD GetFractional60FrameMs(DWORD frameIndex)
{
    // Average:
    //   (16 + 17 + 17) / 3 = 16.666...
    //
    // This gives an exact 60fps long-term cadence while still using
    // millisecond-based timeGetTime timing.
    static const DWORD kPattern[3] = { 16, 17, 17 };
    return kPattern[frameIndex % 3];
}

static DWORD GetTimerFrameMs(Mmx3TimerObject *timer, DWORD frameIndex, bool isMainTimer)
{
#if MMX3_ENABLE_FRACTIONAL_60_EXPERIMENT
    if (isMainTimer) {
        return GetFractional60FrameMs(frameIndex);
    }
#else
    (void)frameIndex;
    (void)isMainTimer;
#endif

    if (!timer) {
        return 1;
    }

    if (timer->intervalMs <= 0) {
        return 1;
    }

    return (DWORD)timer->intervalMs;
}

static void LogFractional60Stats(
    DWORD frameCount,
    DWORD firstFrameTime,
    DWORD lastFrameTime,
    DWORD targetTime,
    DWORD now)
{
#if MMX3_ENABLE_LOG
    if (frameCount == 0) {
        return;
    }

    if ((frameCount % 600) != 0) {
        return;
    }

    double activeFps = 0.0;

    if (frameCount > 1 && lastFrameTime != firstFrameTime) {
        DWORD elapsed = lastFrameTime - firstFrameTime;
        activeFps = (double)(frameCount - 1) * 1000.0 / (double)elapsed;
    }

    LONG driftMs = (LONG)(now - targetTime);

    LogLine(
        "MainTimerFractional60 stats: frames=%lu activeFps=%.4f driftMs=%ld first=%lu last=%lu now=%lu target=%lu",
        (unsigned long)frameCount,
        activeFps,
        (long)driftMs,
        (unsigned long)firstFrameTime,
        (unsigned long)lastFrameTime,
        (unsigned long)now,
        (unsigned long)targetTime);
#else
    (void)frameCount;
    (void)firstFrameTime;
    (void)lastFrameTime;
    (void)targetTime;
    (void)now;
#endif
}

static void __cdecl HookMainTimerThread(void *param)
{
    Mmx3TimerObject *timer = (Mmx3TimerObject *)param;

#if MMX3_ENABLE_LOG
    LogLine(
        "MainTimerFractional60 thread start: timer=%p interval=%ld sleep=%ld callback=%p running=%ld stop=%ld",
        timer,
        timer ? timer->intervalMs : 0,
        timer ? timer->sleepMs : 0,
        timer ? (void *)timer->callback : NULL,
        timer ? timer->running : 0,
        timer ? timer->stopFlag : 0);
#endif

    if (!timer) {
#if MMX3_ENABLE_LOG
        LogLine("MainTimerFractional60 thread end: null timer");
#endif
        _endthread();
        return;
    }

    timer->threadAlive = 1;

    DWORD baseTime = Mmx3RealTimeGetTime();
    DWORD frameIndex = 0;
    DWORD callbackFrameCount = 0;
    DWORD firstFrameTime = 0;
    DWORD lastFrameTime = 0;

    bool wasMainTimer = false;
    bool loggedActivation = false;

    while (timer->running != 0) {
        bool isMainTimer = IsMainTimerObject(timer);

        if (isMainTimer) {
            wasMainTimer = true;
        }

        if (isMainTimer && !loggedActivation) {
            loggedActivation = true;

#if MMX3_ENABLE_LOG
            LogLine(
                "MainTimerFractional60 activated: timer=%p interval=%ld sleep=%ld callback=%p pattern=16/17/17",
                timer,
                timer->intervalMs,
                timer->sleepMs,
                (void *)timer->callback);
#endif
        }

        DWORD frameMs = GetTimerFrameMs(timer, frameIndex, isMainTimer);
        DWORD now = Mmx3RealTimeGetTime();

        if ((LONG)(now - baseTime) < (LONG)frameMs) {
            DWORD sleepMs = 1;

            if (timer->sleepMs > 0) {
                sleepMs = (DWORD)timer->sleepMs;
            }

            do {
                Mmx3RealSleepEx(sleepMs, FALSE);
                now = Mmx3RealTimeGetTime();
            } while ((LONG)(now - baseTime) < (LONG)frameMs);
        }

        baseTime += frameMs;

        // Preserve original catch-up behavior:
        //
        // Original:
        //   if (interval * 0x20 < DVar3 - DVar2)
        //       DVar2 = DVar3 - interval;
        //
        // This avoids the timer trying to replay a huge backlog after stalls.
        if ((LONG)(frameMs * 0x20) < (LONG)(now - baseTime)) {
            baseTime = now - frameMs;
        }

        if (timer->stopFlag == 0 && timer->callback != NULL) {
            timer->callbackBusy = 1;

            timer->callback(timer);

            timer->callbackBusy = 0;

            if (isMainTimer) {
                callbackFrameCount++;

                DWORD callbackTime = Mmx3RealTimeGetTime();

                if (firstFrameTime == 0) {
                    firstFrameTime = callbackTime;
                }

                lastFrameTime = callbackTime;

                LogFractional60Stats(
                    callbackFrameCount,
                    firstFrameTime,
                    lastFrameTime,
                    baseTime,
                    callbackTime);
            }
        }

        frameIndex++;
    }

    timer->threadAlive = 0;

#if MMX3_ENABLE_LOG
    if (wasMainTimer) {
        double activeFps = 0.0;

        if (callbackFrameCount > 1 && lastFrameTime != firstFrameTime) {
            DWORD elapsed = lastFrameTime - firstFrameTime;
            activeFps = (double)(callbackFrameCount - 1) * 1000.0 / (double)elapsed;
        }

        LogLine(
            "MainTimerFractional60 thread end: timer=%p frames=%lu activeFps=%.4f first=%lu last=%lu",
            timer,
            (unsigned long)callbackFrameCount,
            activeFps,
            (unsigned long)firstFrameTime,
            (unsigned long)lastFrameTime);
    } else {
        LogLine("MainTimerFractional60 thread end: non-main timer=%p", timer);
    }
#endif

    _endthread();
}

// ============================================================
// Patch helpers
// ============================================================

static void LogBytes(const char *tag, BYTE *addr, DWORD count)
{
#if MMX3_ENABLE_LOG
    char buf[256];
    char *p = buf;
    char *end = buf + sizeof(buf);

    p += wsprintfA(p, "%s", tag);

    for (DWORD i = 0; i < count && p < end - 4; i++) {
        p += wsprintfA(p, " %02X", addr[i]);
    }

    LogLine("%s", buf);
#else
    (void)tag;
    (void)addr;
    (void)count;
#endif
}

static bool PatchRelativeJump(BYTE *src, void *dst, DWORD patchSize)
{
    if (!src || !dst) {
        return false;
    }

    if (patchSize < 5) {
        return false;
    }

    BYTE patch[16];

    if (patchSize > sizeof(patch)) {
        return false;
    }

    memset(patch, 0x90, sizeof(patch));

    uintptr_t source = (uintptr_t)src;
    uintptr_t target = (uintptr_t)dst;

    int32_t rel = (int32_t)(target - (source + 5));

    patch[0] = 0xE9;
    memcpy(patch + 1, &rel, sizeof(rel));

    return PatchMemory(src, patch, patchSize) != 0;
}

// ============================================================
// Timer experiment installers
// ============================================================

static void InstallMainTimer16msExperiment(HMODULE exe)
{
    (void)exe;

#if MMX3_ENABLE_16MS_TIMER_EXPERIMENT
    BYTE *p = MMX3_ADDR_MAIN_TIMER_PUSH_MS;

    if (p[0] != 0x6A || p[1] != 0x11) {
        LogLine(
            "MainTimer16msExperiment skipped: unexpected bytes at 00402354: %02X %02X",
            p[0],
            p[1]);
        return;
    }

    BYTE patch[] = { 0x6A, 0x10 };

    if (PatchMemory(p, patch, sizeof(patch))) {
        LogLine("MainTimer16msExperiment applied: 00402354 push 11h -> push 10h");
    } else {
        LogLine("MainTimer16msExperiment failed");
    }
#else
    (void)exe;
#endif
}

static void InstallMainTimerFractional60Experiment(HMODULE exe)
{
    (void)exe;

    if (!g_patchConfig.fractional60FpsTimer) {
        LogLine("MainTimerFractional60Experiment skipped: disabled by MMX3.conf");
        return;
    }

#if MMX3_ENABLE_FRACTIONAL_60_EXPERIMENT
    BYTE *p = MMX3_ADDR_MAIN_TIMER_THREAD;

    LogBytes("MainTimerFractional60Experiment check: 004DEB10 bytes=", p, 8);

    if (memcmp(p, kMainTimerThreadPrologue, sizeof(kMainTimerThreadPrologue)) != 0) {
        LogLine(
            "MainTimerFractional60Experiment skipped: unexpected prologue at 004DEB10: %02X %02X %02X %02X %02X %02X %02X %02X",
            p[0],
            p[1],
            p[2],
            p[3],
            p[4],
            p[5],
            p[6],
            p[7]);
        return;
    }

    // We overwrite:
    //   53 56 8B 74 24 0C
    //
    // with:
    //   E9 xx xx xx xx 90
    //
    // Six bytes is enough because the first three instructions are:
    //   push ebx              1 byte
    //   push esi              1 byte
    //   mov esi,[esp+0Ch]     4 bytes
    //
    // We do not need a trampoline because this experiment fully replaces
    // the original timer thread body.
    const DWORD patchSize = 6;

    if (PatchRelativeJump(p, (void *)HookMainTimerThread, patchSize)) {
        LogLine(
            "MainTimerFractional60Experiment applied: 004DEB10 -> HookMainTimerThread, patchSize=%lu, pattern=16/17/17",
            (unsigned long)patchSize);
    } else {
        LogLine("MainTimerFractional60Experiment failed");
    }
#else
    (void)exe;
#endif
}

static void InstallTimerExperiments(HMODULE exe)
{
#if MMX3_TIMER_MODE == MMX3_TIMER_MODE_ORIGINAL
    LogLine("InstallTimerExperiments: mode=ORIGINAL");
#elif MMX3_TIMER_MODE == MMX3_TIMER_MODE_PATCH_16MS
    LogLine("InstallTimerExperiments: mode=PATCH_16MS");
    InstallMainTimer16msExperiment(exe);
#elif MMX3_TIMER_MODE == MMX3_TIMER_MODE_FRACTIONAL_60
    LogLine("InstallTimerExperiments: mode=FRACTIONAL_60");
    InstallMainTimerFractional60Experiment(exe);
#else
    LogLine("InstallTimerExperiments: unknown MMX3_TIMER_MODE=%d", MMX3_TIMER_MODE);
#endif
}

// ============================================================
// Public installer
// ============================================================

void InstallTimingHooks(HMODULE exe)
{
#if MMX3_ENABLE_TIMING_LOG
    LogLine("InstallTimingHooks");
#else
    (void)exe;
    return;
#endif

    if (!exe) {
        LogLine("InstallTimingHooks skipped: exe is null");
        return;
    }

    if (!PatchIAT(
            exe,
            "KERNEL32.DLL",
            "Sleep",
            (void *)HookSleep,
            (void **)&g_realSleep)) {
        LogLine("Timing hook skipped: KERNEL32.DLL!Sleep");
    }

    if (!PatchIAT(
            exe,
            "KERNEL32.DLL",
            "SleepEx",
            (void *)HookSleepEx,
            (void **)&g_realSleepEx)) {
        LogLine("Timing hook skipped: KERNEL32.DLL!SleepEx");
    }

    if (!PatchIAT(
            exe,
            "KERNEL32.DLL",
            "GetTickCount",
            (void *)HookGetTickCount,
            (void **)&g_realGetTickCount)) {
        LogLine("Timing hook skipped: KERNEL32.DLL!GetTickCount");
    }

    if (!PatchIAT(
            exe,
            "KERNEL32.DLL",
            "QueryPerformanceCounter",
            (void *)HookQueryPerformanceCounter,
            (void **)&g_realQueryPerformanceCounter)) {
        LogLine("Timing hook skipped: KERNEL32.DLL!QueryPerformanceCounter");
    }

    if (!PatchIAT(
            exe,
            "KERNEL32.DLL",
            "QueryPerformanceFrequency",
            (void *)HookQueryPerformanceFrequency,
            (void **)&g_realQueryPerformanceFrequency)) {
        LogLine("Timing hook skipped: KERNEL32.DLL!QueryPerformanceFrequency");
    }

    if (!PatchIAT(
            exe,
            "WINMM.DLL",
            "timeGetTime",
            (void *)HookTimeGetTime,
            (void **)&g_realTimeGetTime)) {
        LogLine("Timing hook skipped: WINMM.DLL!timeGetTime");
    }

    InstallTimerExperiments(exe);
}