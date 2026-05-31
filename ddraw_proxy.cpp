#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>

// ============================================================
// Real DLL / real API pointers
// ============================================================

static HMODULE g_realDdraw = NULL;

typedef LONG (WINAPI *RegOpenKeyExA_t)(
    HKEY hKey,
    LPCSTR lpSubKey,
    DWORD ulOptions,
    REGSAM samDesired,
    PHKEY phkResult);

typedef LONG (WINAPI *RegCreateKeyExA_t)(
    HKEY hKey,
    LPCSTR lpSubKey,
    DWORD Reserved,
    LPSTR lpClass,
    DWORD dwOptions,
    REGSAM samDesired,
    const LPSECURITY_ATTRIBUTES lpSecurityAttributes,
    PHKEY phkResult,
    LPDWORD lpdwDisposition);

typedef LONG (WINAPI *RegQueryValueExA_t)(
    HKEY hKey,
    LPCSTR lpValueName,
    LPDWORD lpReserved,
    LPDWORD lpType,
    LPBYTE lpData,
    LPDWORD lpcbData);

typedef LONG (WINAPI *RegSetValueExA_t)(
    HKEY hKey,
    LPCSTR lpValueName,
    DWORD Reserved,
    DWORD dwType,
    const BYTE *lpData,
    DWORD cbData);

typedef LONG (WINAPI *RegCloseKey_t)(
    HKEY hKey);

typedef UINT (WINAPI *GetDriveTypeA_t)(
    LPCSTR lpRootPathName);

static RegOpenKeyExA_t       g_realRegOpenKeyExA      = NULL;
static RegCreateKeyExA_t     g_realRegCreateKeyExA    = NULL;
static RegQueryValueExA_t    g_realRegQueryValueExA   = NULL;
static RegSetValueExA_t      g_realRegSetValueExA     = NULL;
static RegCloseKey_t         g_realRegCloseKey        = NULL;
static GetDriveTypeA_t       g_realGetDriveTypeA      = NULL;

// ============================================================
// Fake registry handles
// ============================================================

#define FAKE_HKEY_MMX3          ((HKEY)(ULONG_PTR)0x12345001)
#define FAKE_HKEY_MMX3_CARD     ((HKEY)(ULONG_PTR)0x12345002)
#define FAKE_HKEY_MMX3_KEYCFG   ((HKEY)(ULONG_PTR)0x12345003)

// ============================================================
// Paths
// ============================================================

static char g_gameDir[MAX_PATH];
static char g_gameDriveRoot[4];

static char g_passwordPath[MAX_PATH];
static char g_keyGamePadPath[MAX_PATH];
static char g_keyKeyboardPath[MAX_PATH];
static char g_keySideWinderPath[MAX_PATH];

static void BuildPaths()
{
    GetModuleFileNameA(NULL, g_gameDir, MAX_PATH);

    char *slash = strrchr(g_gameDir, '\\');
    if (slash) {
        slash[1] = '\0';
    }

    if (g_gameDir[0] && g_gameDir[1] == ':') {
        g_gameDriveRoot[0] = g_gameDir[0];
        g_gameDriveRoot[1] = ':';
        g_gameDriveRoot[2] = '\\';
        g_gameDriveRoot[3] = '\0';
    } else {
        g_gameDriveRoot[0] = '\0';
    }

    lstrcpynA(g_passwordPath, g_gameDir, MAX_PATH);
    lstrcatA(g_passwordPath, "Password.sav");

    lstrcpynA(g_keyGamePadPath, g_gameDir, MAX_PATH);
    lstrcatA(g_keyGamePadPath, "KeyConfig_GamePad.bin");

    lstrcpynA(g_keyKeyboardPath, g_gameDir, MAX_PATH);
    lstrcatA(g_keyKeyboardPath, "KeyConfig_Keyboard.bin");

    lstrcpynA(g_keySideWinderPath, g_gameDir, MAX_PATH);
    lstrcatA(g_keySideWinderPath, "KeyConfig_SideWinder.bin");
}

// ============================================================
// Default data
// ============================================================

static const BYTE kDefaultPassword[0x80] = {0};

static const BYTE kDefaultGamePad[] = {
    0x00,0x00, 0x01,0x00, 0x02,0x00, 0x03,0x00,
    0x04,0x00, 0x05,0x00, 0x06,0x00, 0x07,0x00,
    0x08,0x00, 0x09,0x00, 0x0c,0x00, 0x0d,0x00,
    0x0a,0x00
};

static const BYTE kDefaultKeyboard[] = {
    0x26,0x00, 0x28,0x00, 0x25,0x00, 0x27,0x00,
    0x5a,0x00, 0x58,0x00, 0x43,0x00, 0x41,0x00,
    0x53,0x00, 0x44,0x00, 0x44,0x00
};

static const BYTE kDefaultSideWinder[] = {
    0x00,0x00, 0x01,0x00, 0x02,0x00, 0x03,0x00,
    0x04,0x00, 0x05,0x00, 0x06,0x00, 0x07,0x00,
    0x08,0x00, 0x09,0x00, 0x0c,0x00, 0x0c,0x00,
    0x0a,0x00, 0x0b,0x00
};

// ============================================================
// Utility helpers
// ============================================================

static bool IsValue(LPCSTR a, LPCSTR b)
{
    return a && b && lstrcmpiA(a, b) == 0;
}

static bool ContainsNoCase(const char *s, const char *needle)
{
    if (!s || !needle) {
        return false;
    }

    char a[MAX_PATH * 2];
    char b[MAX_PATH * 2];

    lstrcpynA(a, s, sizeof(a));
    lstrcpynA(b, needle, sizeof(b));

    CharLowerA(a);
    CharLowerA(b);

    return strstr(a, b) != NULL;
}

static bool IsMMX3RootKeyPath(LPCSTR subKey)
{
    if (!subKey) {
        return false;
    }

    if (!ContainsNoCase(subKey, "Software\\CAPCOM\\MEGAMANX3")) {
        return false;
    }

    return !ContainsNoCase(subKey, "\\Card") &&
           !ContainsNoCase(subKey, "\\KeyConfig");
}

static bool IsMMX3CardKeyPath(LPCSTR subKey)
{
    return subKey &&
           ContainsNoCase(subKey, "Software\\CAPCOM\\MEGAMANX3\\Card");
}

static bool IsMMX3KeyConfigPath(LPCSTR subKey)
{
    return subKey &&
           ContainsNoCase(subKey, "Software\\CAPCOM\\MEGAMANX3\\KeyConfig");
}

static LONG ReturnRegSz(
    LPCSTR value,
    LPDWORD lpType,
    LPBYTE lpData,
    LPDWORD lpcbData)
{
    if (!value || !lpcbData) {
        return ERROR_INVALID_PARAMETER;
    }

    DWORD need = lstrlenA(value) + 1;

    if (lpType) {
        *lpType = REG_SZ;
    }

    if (!lpData) {
        *lpcbData = need;
        return ERROR_SUCCESS;
    }

    if (*lpcbData < need) {
        *lpcbData = need;
        return ERROR_MORE_DATA;
    }

    memcpy(lpData, value, need);
    *lpcbData = need;
    return ERROR_SUCCESS;
}

static LONG ReturnRegBinary(
    const BYTE *src,
    DWORD srcSize,
    LPDWORD lpType,
    LPBYTE lpData,
    LPDWORD lpcbData)
{
    if (!src || !lpcbData) {
        return ERROR_INVALID_PARAMETER;
    }

    if (lpType) {
        *lpType = REG_BINARY;
    }

    if (!lpData) {
        *lpcbData = srcSize;
        return ERROR_SUCCESS;
    }

    if (*lpcbData < srcSize) {
        *lpcbData = srcSize;
        return ERROR_MORE_DATA;
    }

    memcpy(lpData, src, srcSize);
    *lpcbData = srcSize;
    return ERROR_SUCCESS;
}

static LONG ReturnBinaryFileOrDefault(
    const char *path,
    const BYTE *defaultData,
    DWORD defaultSize,
    LPDWORD lpType,
    LPBYTE lpData,
    LPDWORD lpcbData)
{
    if (!path || !defaultData || !lpcbData) {
        return ERROR_INVALID_PARAMETER;
    }

    FILE *fp = fopen(path, "rb");

    if (!fp) {
        return ReturnRegBinary(defaultData, defaultSize, lpType, lpData, lpcbData);
    }

    BYTE temp[512];
    memset(temp, 0, sizeof(temp));

    size_t n = fread(temp, 1, sizeof(temp), fp);
    fclose(fp);

    if (n == 0) {
        return ReturnRegBinary(defaultData, defaultSize, lpType, lpData, lpcbData);
    }

    return ReturnRegBinary(temp, (DWORD)n, lpType, lpData, lpcbData);
}

static LONG WriteBinaryFile(const char *path, const BYTE *data, DWORD size)
{
    if (!path || !data || size == 0) {
        return ERROR_INVALID_PARAMETER;
    }

    FILE *fp = fopen(path, "wb");

    if (!fp) {
        return ERROR_ACCESS_DENIED;
    }

    size_t written = fwrite(data, 1, size, fp);
    fclose(fp);

    return written == size ? ERROR_SUCCESS : ERROR_WRITE_FAULT;
}

static bool PatchMemory(void *address, const BYTE *patch, SIZE_T patchSize)
{
    DWORD oldProtect;

    if (!VirtualProtect(address, patchSize, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        return false;
    }

    memcpy(address, patch, patchSize);
    FlushInstructionCache(GetCurrentProcess(), address, patchSize);

    DWORD dummy;
    VirtualProtect(address, patchSize, oldProtect, &dummy);

    return true;
}

// ============================================================
// Internal EXE patches
// ============================================================

static void PatchCdAudioInitCheck()
{
    HMODULE exe = GetModuleHandleA(NULL);

    if (!exe) {
        return;
    }

    // Ghidra VA : 0x004DF250
    // ImageBase : 0x00400000
    // RVA       : 0x000DF250
    BYTE *target = (BYTE *)exe + 0x000DF250;

    // FUN_004DF250 calling convention:
    //   __thiscall, 2 stack args, returns with RET 8
    //
    // Patch:
    //   mov eax, 1
    //   ret 8
    static const BYTE patch[] = {
        0xB8, 0x01, 0x00, 0x00, 0x00,
        0xC2, 0x08, 0x00
    };

    PatchMemory(target, patch, sizeof(patch));
}

static void PatchCdResourceCheck()
{
    HMODULE exe = GetModuleHandleA(NULL);

    if (!exe) {
        return;
    }

    // Ghidra VA : 0x00404090
    // ImageBase : 0x00400000
    // RVA       : 0x00004090
    BYTE *target = (BYTE *)exe + 0x00004090;

    // mov eax, 0x2a01
    // ret 4
    static const BYTE patch[] = {
        0xB8, 0x01, 0x2A, 0x00, 0x00,
        0xC2, 0x04, 0x00
    };

    PatchMemory(target, patch, sizeof(patch));
}

// ============================================================
// KERNEL32 hook
// ============================================================

static UINT WINAPI MyGetDriveTypeA(LPCSTR lpRootPathName)
{
    if (lpRootPathName && g_gameDriveRoot[0]) {
        // Exact match: "D:\"
        if (lstrcmpiA(lpRootPathName, g_gameDriveRoot) == 0) {
            return DRIVE_CDROM;
        }

        // Loose match: "D:" or any path on same drive.
        if (lpRootPathName[0] &&
            lpRootPathName[1] == ':' &&
            (lpRootPathName[0] == g_gameDriveRoot[0] ||
             lpRootPathName[0] == (char)(g_gameDriveRoot[0] + 32) ||
             lpRootPathName[0] == (char)(g_gameDriveRoot[0] - 32))) {
            return DRIVE_CDROM;
        }
    }

    return g_realGetDriveTypeA
        ? g_realGetDriveTypeA(lpRootPathName)
        : DRIVE_UNKNOWN;
}

// ============================================================
// Registry hooks
// ============================================================

static LONG WINAPI MyRegOpenKeyExA(
    HKEY hKey,
    LPCSTR lpSubKey,
    DWORD ulOptions,
    REGSAM samDesired,
    PHKEY phkResult)
{
    if (phkResult && IsMMX3CardKeyPath(lpSubKey)) {
        *phkResult = FAKE_HKEY_MMX3_CARD;
        return ERROR_SUCCESS;
    }

    if (phkResult && IsMMX3KeyConfigPath(lpSubKey)) {
        *phkResult = FAKE_HKEY_MMX3_KEYCFG;
        return ERROR_SUCCESS;
    }

    if (phkResult && IsMMX3RootKeyPath(lpSubKey)) {
        *phkResult = FAKE_HKEY_MMX3;
        return ERROR_SUCCESS;
    }

    return g_realRegOpenKeyExA(hKey, lpSubKey, ulOptions, samDesired, phkResult);
}

static LONG WINAPI MyRegCreateKeyExA(
    HKEY hKey,
    LPCSTR lpSubKey,
    DWORD Reserved,
    LPSTR lpClass,
    DWORD dwOptions,
    REGSAM samDesired,
    const LPSECURITY_ATTRIBUTES lpSecurityAttributes,
    PHKEY phkResult,
    LPDWORD lpdwDisposition)
{
    if (phkResult && IsMMX3CardKeyPath(lpSubKey)) {
        *phkResult = FAKE_HKEY_MMX3_CARD;
        if (lpdwDisposition) {
            *lpdwDisposition = REG_OPENED_EXISTING_KEY;
        }
        return ERROR_SUCCESS;
    }

    if (phkResult && IsMMX3KeyConfigPath(lpSubKey)) {
        *phkResult = FAKE_HKEY_MMX3_KEYCFG;
        if (lpdwDisposition) {
            *lpdwDisposition = REG_OPENED_EXISTING_KEY;
        }
        return ERROR_SUCCESS;
    }

    if (phkResult && IsMMX3RootKeyPath(lpSubKey)) {
        *phkResult = FAKE_HKEY_MMX3;
        if (lpdwDisposition) {
            *lpdwDisposition = REG_OPENED_EXISTING_KEY;
        }
        return ERROR_SUCCESS;
    }

    return g_realRegCreateKeyExA(
        hKey,
        lpSubKey,
        Reserved,
        lpClass,
        dwOptions,
        samDesired,
        lpSecurityAttributes,
        phkResult,
        lpdwDisposition);
}

static LONG WINAPI MyRegQueryValueExA(
    HKEY hKey,
    LPCSTR lpValueName,
    LPDWORD lpReserved,
    LPDWORD lpType,
    LPBYTE lpData,
    LPDWORD lpcbData)
{
    if (hKey == FAKE_HKEY_MMX3) {
        if (IsValue(lpValueName, "CD Drive") ||
            IsValue(lpValueName, "Install Dir")) {
            return ReturnRegSz(g_gameDir, lpType, lpData, lpcbData);
        }

        if (IsValue(lpValueName, "Inst Program") ||
            IsValue(lpValueName, "Inst Resource") ||
            IsValue(lpValueName, "Inst Movie") ||
            IsValue(lpValueName, "Inst BGM") ||
            IsValue(lpValueName, "DoubleView") ||
            IsValue(lpValueName, "Easy Mode")) {
            return ReturnRegSz("True", lpType, lpData, lpcbData);
        }

        if (IsValue(lpValueName, "FullScreen")) {
            return ReturnRegSz("False", lpType, lpData, lpcbData);
        }

        if (IsValue(lpValueName, "Screen Mode")) {
            return ReturnRegSz("640,480,8", lpType, lpData, lpcbData);
        }

        return ERROR_FILE_NOT_FOUND;
    }

    if (hKey == FAKE_HKEY_MMX3_CARD) {
        if (IsValue(lpValueName, "Password")) {
            return ReturnBinaryFileOrDefault(
                g_passwordPath,
                kDefaultPassword,
                sizeof(kDefaultPassword),
                lpType,
                lpData,
                lpcbData);
        }

        return ERROR_FILE_NOT_FOUND;
    }

    if (hKey == FAKE_HKEY_MMX3_KEYCFG) {
        if (IsValue(lpValueName, "GamePad")) {
            return ReturnBinaryFileOrDefault(
                g_keyGamePadPath,
                kDefaultGamePad,
                sizeof(kDefaultGamePad),
                lpType,
                lpData,
                lpcbData);
        }

        if (IsValue(lpValueName, "Keyboard")) {
            return ReturnBinaryFileOrDefault(
                g_keyKeyboardPath,
                kDefaultKeyboard,
                sizeof(kDefaultKeyboard),
                lpType,
                lpData,
                lpcbData);
        }

        if (IsValue(lpValueName, "SideWinder")) {
            return ReturnBinaryFileOrDefault(
                g_keySideWinderPath,
                kDefaultSideWinder,
                sizeof(kDefaultSideWinder),
                lpType,
                lpData,
                lpcbData);
        }

        return ERROR_FILE_NOT_FOUND;
    }

    return g_realRegQueryValueExA(
        hKey,
        lpValueName,
        lpReserved,
        lpType,
        lpData,
        lpcbData);
}

static LONG WINAPI MyRegSetValueExA(
    HKEY hKey,
    LPCSTR lpValueName,
    DWORD Reserved,
    DWORD dwType,
    const BYTE *lpData,
    DWORD cbData)
{
    if (hKey == FAKE_HKEY_MMX3_CARD) {
        if (IsValue(lpValueName, "Password") &&
            dwType == REG_BINARY &&
            lpData &&
            cbData >= 0x80) {
            return WriteBinaryFile(g_passwordPath, lpData, 0x80);
        }

        return ERROR_SUCCESS;
    }

    if (hKey == FAKE_HKEY_MMX3_KEYCFG) {
        if (IsValue(lpValueName, "GamePad") &&
            dwType == REG_BINARY &&
            lpData &&
            cbData > 0) {
            return WriteBinaryFile(g_keyGamePadPath, lpData, cbData);
        }

        if (IsValue(lpValueName, "Keyboard") &&
            dwType == REG_BINARY &&
            lpData &&
            cbData > 0) {
            return WriteBinaryFile(g_keyKeyboardPath, lpData, cbData);
        }

        if (IsValue(lpValueName, "SideWinder") &&
            dwType == REG_BINARY &&
            lpData &&
            cbData > 0) {
            return WriteBinaryFile(g_keySideWinderPath, lpData, cbData);
        }

        return ERROR_SUCCESS;
    }

    if (hKey == FAKE_HKEY_MMX3) {
        return ERROR_SUCCESS;
    }

    return g_realRegSetValueExA(
        hKey,
        lpValueName,
        Reserved,
        dwType,
        lpData,
        cbData);
}

static LONG WINAPI MyRegCloseKey(HKEY hKey)
{
    if (hKey == FAKE_HKEY_MMX3 ||
        hKey == FAKE_HKEY_MMX3_CARD ||
        hKey == FAKE_HKEY_MMX3_KEYCFG) {
        return ERROR_SUCCESS;
    }

    return g_realRegCloseKey(hKey);
}

// ============================================================
// IAT patch
// ============================================================

static bool PatchIAT(
    HMODULE module,
    const char *dllName,
    const char *funcName,
    void *newFunc,
    void **oldFunc)
{
    BYTE *base = (BYTE *)module;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;

    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return false;
    }

    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);

    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return false;
    }

    IMAGE_DATA_DIRECTORY importDir =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];

    if (!importDir.VirtualAddress) {
        return false;
    }

    IMAGE_IMPORT_DESCRIPTOR *desc =
        (IMAGE_IMPORT_DESCRIPTOR *)(base + importDir.VirtualAddress);

    for (; desc->Name; desc++) {
        const char *importDll = (const char *)(base + desc->Name);

        if (lstrcmpiA(importDll, dllName) != 0) {
            continue;
        }

        if (!desc->OriginalFirstThunk || !desc->FirstThunk) {
            continue;
        }

        IMAGE_THUNK_DATA *origThunk =
            (IMAGE_THUNK_DATA *)(base + desc->OriginalFirstThunk);

        IMAGE_THUNK_DATA *firstThunk =
            (IMAGE_THUNK_DATA *)(base + desc->FirstThunk);

        for (; origThunk->u1.AddressOfData; origThunk++, firstThunk++) {
            if (origThunk->u1.Ordinal & IMAGE_ORDINAL_FLAG) {
                continue;
            }

            IMAGE_IMPORT_BY_NAME *importName =
                (IMAGE_IMPORT_BY_NAME *)(base + origThunk->u1.AddressOfData);

            if (lstrcmpA((const char *)importName->Name, funcName) == 0) {
                DWORD oldProtect;

                if (!VirtualProtect(
                        &firstThunk->u1.Function,
                        sizeof(void *),
                        PAGE_READWRITE,
                        &oldProtect)) {
                    return false;
                }

                if (oldFunc) {
                    *oldFunc = (void *)firstThunk->u1.Function;
                }

                firstThunk->u1.Function = (ULONG_PTR)newFunc;

                VirtualProtect(
                    &firstThunk->u1.Function,
                    sizeof(void *),
                    oldProtect,
                    &oldProtect);

                return true;
            }
        }
    }

    return false;
}

// ============================================================
// Real ddraw loader / proxy helpers
// ============================================================

static void LoadRealDdraw()
{
    if (g_realDdraw) {
        return;
    }

    char sysPath[MAX_PATH];
    GetSystemDirectoryA(sysPath, MAX_PATH);
    lstrcatA(sysPath, "\\ddraw.dll");

    g_realDdraw = LoadLibraryA(sysPath);
}

static FARPROC GetRealDdrawProc(const char *name)
{
    LoadRealDdraw();

    if (!g_realDdraw) {
        return NULL;
    }

    return GetProcAddress(g_realDdraw, name);
}

// ============================================================
// Init
// ============================================================

static DWORD WINAPI InitThread(void *)
{
    BuildPaths();

    // Patch internal CD audio init/check first.
    PatchCdAudioInitCheck();
	PatchCdResourceCheck();
    HMODULE advapi = GetModuleHandleA("ADVAPI32.DLL");
    if (!advapi) {
        advapi = LoadLibraryA("ADVAPI32.DLL");
    }

    HMODULE kernel32 = GetModuleHandleA("KERNEL32.DLL");
    if (!kernel32) {
        kernel32 = LoadLibraryA("KERNEL32.DLL");
    }

    HMODULE exe = GetModuleHandleA(NULL);

    if (advapi) {
        g_realRegOpenKeyExA =
            (RegOpenKeyExA_t)GetProcAddress(advapi, "RegOpenKeyExA");

        g_realRegCreateKeyExA =
            (RegCreateKeyExA_t)GetProcAddress(advapi, "RegCreateKeyExA");

        g_realRegQueryValueExA =
            (RegQueryValueExA_t)GetProcAddress(advapi, "RegQueryValueExA");

        g_realRegSetValueExA =
            (RegSetValueExA_t)GetProcAddress(advapi, "RegSetValueExA");

        g_realRegCloseKey =
            (RegCloseKey_t)GetProcAddress(advapi, "RegCloseKey");

        PatchIAT(exe, "ADVAPI32.DLL", "RegOpenKeyExA",
                 (void *)MyRegOpenKeyExA,
                 (void **)&g_realRegOpenKeyExA);

        PatchIAT(exe, "ADVAPI32.DLL", "RegCreateKeyExA",
                 (void *)MyRegCreateKeyExA,
                 (void **)&g_realRegCreateKeyExA);

        PatchIAT(exe, "ADVAPI32.DLL", "RegQueryValueExA",
                 (void *)MyRegQueryValueExA,
                 (void **)&g_realRegQueryValueExA);

        PatchIAT(exe, "ADVAPI32.DLL", "RegSetValueExA",
                 (void *)MyRegSetValueExA,
                 (void **)&g_realRegSetValueExA);

        PatchIAT(exe, "ADVAPI32.DLL", "RegCloseKey",
                 (void *)MyRegCloseKey,
                 (void **)&g_realRegCloseKey);
    }

    if (kernel32) {
        g_realGetDriveTypeA =
            (GetDriveTypeA_t)GetProcAddress(kernel32, "GetDriveTypeA");

        PatchIAT(exe, "KERNEL32.DLL", "GetDriveTypeA",
                 (void *)MyGetDriveTypeA,
                 (void **)&g_realGetDriveTypeA);
    }

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

// ============================================================
// ddraw.dll proxy exports
// If the game imports more DDRAW exports, add them here.
// ============================================================

extern "C" __declspec(dllexport)
HRESULT WINAPI DirectDrawCreate(void *lpGUID, void *lplpDD, void *pUnkOuter)
{
    typedef HRESULT (WINAPI *Fn)(void *, void *, void *);
    Fn fn = (Fn)GetRealDdrawProc("DirectDrawCreate");
    return fn ? fn(lpGUID, lplpDD, pUnkOuter) : E_FAIL;
}

extern "C" __declspec(dllexport)
HRESULT WINAPI DirectDrawCreateEx(void *lpGUID, void *lplpDD, const void *iid, void *pUnkOuter)
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
HRESULT WINAPI DirectDrawCreateClipper(DWORD flags, void *lplpDDClipper, void *pUnkOuter)
{
    typedef HRESULT (WINAPI *Fn)(DWORD, void *, void *);
    Fn fn = (Fn)GetRealDdrawProc("DirectDrawCreateClipper");
    return fn ? fn(flags, lplpDDClipper, pUnkOuter) : E_FAIL;
}