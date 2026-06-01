#include "mmx3_common.h"

static HMODULE g_realDdraw = NULL;

static void LoadRealDdraw()
{
    if (g_realDdraw) {
        return;
    }

    char sysPath[MAX_PATH];
    GetSystemDirectoryA(sysPath, MAX_PATH);
    lstrcatA(sysPath, "\\ddraw.dll");

    g_realDdraw = LoadLibraryA(sysPath);
    LogLine("LoadRealDdraw: %s result=%p", sysPath, g_realDdraw);
}

static FARPROC GetRealDdrawProc(const char *name)
{
    LoadRealDdraw();

    if (!g_realDdraw) {
        LogLine("GetRealDdrawProc failed: %s", name);
        return NULL;
    }

    FARPROC proc = GetProcAddress(g_realDdraw, name);
    LogLine("GetRealDdrawProc: %s -> %p", name, proc);
    return proc;
}


// ============================================================
// Crash logger
// ============================================================

static LONG WINAPI CrashHandler(EXCEPTION_POINTERS *ep)
{
    if (!ep || !ep->ExceptionRecord || !ep->ContextRecord) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    CONTEXT *ctx = ep->ContextRecord;

    LogLine("=== Unhandled exception ===");
    LogLine("ExceptionCode=0x%08lX Address=%p",
            (unsigned long)er->ExceptionCode,
            er->ExceptionAddress);

    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
        er->NumberParameters >= 2) {
        LogLine("AccessViolation type=%s address=%p",
                er->ExceptionInformation[0] ? "write" : "read",
                (void *)er->ExceptionInformation[1]);
    }

#if defined(_M_IX86)
    LogLine("EAX=%08lX EBX=%08lX ECX=%08lX EDX=%08lX",
            (unsigned long)ctx->Eax,
            (unsigned long)ctx->Ebx,
            (unsigned long)ctx->Ecx,
            (unsigned long)ctx->Edx);
    LogLine("ESI=%08lX EDI=%08lX EBP=%08lX ESP=%08lX",
            (unsigned long)ctx->Esi,
            (unsigned long)ctx->Edi,
            (unsigned long)ctx->Ebp,
            (unsigned long)ctx->Esp);
    LogLine("EIP=%08lX EFLAGS=%08lX",
            (unsigned long)ctx->Eip,
            (unsigned long)ctx->EFlags);
#else
    LogLine("Crash register dump is enabled for x86 builds only.");
#endif

    return EXCEPTION_CONTINUE_SEARCH;
}

static DWORD WINAPI InitThread(void *)
{
    BuildPaths();

    SetUnhandledExceptionFilter(CrashHandler);
    LogLine("CrashHandler installed");

    HMODULE exe = GetModuleHandleA(NULL);

    LogLine("Init exe=%p", exe);

	InstallTimingHooks(exe);
    InstallRegistryHooks(exe);
    InstallCdHooks(exe);
    InstallBugFixes(exe);
    LogLine("InitThread done");
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinst);
        CreateThread(NULL, 0, InitThread, NULL, 0, NULL);
    }

    return TRUE;
}

extern "C" __declspec(dllexport)
HRESULT WINAPI DirectDrawCreate(void *lpGUID, void *lplpDD, void *pUnkOuter)
{
    typedef HRESULT (WINAPI *Fn)(void *, void *, void *);
    Fn fn = (Fn)GetRealDdrawProc("DirectDrawCreate");
    return fn ? fn(lpGUID, lplpDD, pUnkOuter) : E_FAIL;
}

extern "C" __declspec(dllexport)
HRESULT WINAPI DirectDrawCreateEx(
    void *lpGUID,
    void *lplpDD,
    const void *iid,
    void *pUnkOuter)
{
    typedef HRESULT (WINAPI *Fn)(void *, void *, const void *, void *);
    Fn fn = (Fn)GetRealDdrawProc("DirectDrawCreateEx");
    return fn ? fn(lpGUID, lplpDD, iid, pUnkOuter) : E_FAIL;
}

extern "C" __declspec(dllexport)
HRESULT WINAPI DirectDrawEnumerateA(void *callback, void *context)
{
    typedef HRESULT (WINAPI *Fn)(void *, void *);
    Fn fn = (Fn)GetRealDdrawProc("DirectDrawEnumerateA");
    return fn ? fn(callback, context) : E_FAIL;
}

extern "C" __declspec(dllexport)
HRESULT WINAPI DirectDrawEnumerateW(void *callback, void *context)
{
    typedef HRESULT (WINAPI *Fn)(void *, void *);
    Fn fn = (Fn)GetRealDdrawProc("DirectDrawEnumerateW");
    return fn ? fn(callback, context) : E_FAIL;
}

extern "C" __declspec(dllexport)
HRESULT WINAPI DirectDrawEnumerateExA(void *callback, void *context, DWORD flags)
{
    typedef HRESULT (WINAPI *Fn)(void *, void *, DWORD);
    Fn fn = (Fn)GetRealDdrawProc("DirectDrawEnumerateExA");
    return fn ? fn(callback, context, flags) : E_FAIL;
}

extern "C" __declspec(dllexport)
HRESULT WINAPI DirectDrawEnumerateExW(void *callback, void *context, DWORD flags)
{
    typedef HRESULT (WINAPI *Fn)(void *, void *, DWORD);
    Fn fn = (Fn)GetRealDdrawProc("DirectDrawEnumerateExW");
    return fn ? fn(callback, context, flags) : E_FAIL;
}

extern "C" __declspec(dllexport)
HRESULT WINAPI DirectDrawCreateClipper(
    DWORD flags,
    void *lplpDDClipper,
    void *pUnkOuter)
{
    typedef HRESULT (WINAPI *Fn)(DWORD, void *, void *);
    Fn fn = (Fn)GetRealDdrawProc("DirectDrawCreateClipper");
    return fn ? fn(flags, lplpDDClipper, pUnkOuter) : E_FAIL;
}
