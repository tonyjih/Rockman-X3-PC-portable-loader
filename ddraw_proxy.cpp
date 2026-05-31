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

static DWORD WINAPI InitThread(void *)
{
    BuildPaths();

    HMODULE exe = GetModuleHandleA(NULL);

    LogLine("Init exe=%p", exe);

    InstallRegistryHooks(exe);
    InstallCdHooks(exe);

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
