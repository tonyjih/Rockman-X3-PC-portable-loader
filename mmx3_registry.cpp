#include "mmx3_common.h"

#include <stdio.h>
#include <string.h>

// Real registry API pointers

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

typedef LONG (WINAPI *RegCloseKey_t)(HKEY hKey);

static RegOpenKeyExA_t       g_realRegOpenKeyExA    = NULL;
static RegCreateKeyExA_t     g_realRegCreateKeyExA  = NULL;
static RegQueryValueExA_t    g_realRegQueryValueExA = NULL;
static RegSetValueExA_t      g_realRegSetValueExA   = NULL;
static RegCloseKey_t         g_realRegCloseKey      = NULL;

// Fake registry handles

#define FAKE_HKEY_MMX3          ((HKEY)(ULONG_PTR)0x12345001)
#define FAKE_HKEY_MMX3_CARD     ((HKEY)(ULONG_PTR)0x12345002)
#define FAKE_HKEY_MMX3_KEYCFG   ((HKEY)(ULONG_PTR)0x12345003)

// Defaults

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

// Helpers

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

static bool IsMMX3BaseKeyPath(LPCSTR subKey)
{
    if (!subKey) {
        return false;
    }

    return ContainsNoCase(subKey, "Software\\CAPCOM\\MEGAMANX3") ||
           ContainsNoCase(subKey, "Software\\CAPCOM\\ROCKMANX3");
}

static bool IsMMX3RootKeyPath(LPCSTR subKey)
{
    if (!IsMMX3BaseKeyPath(subKey)) {
        return false;
    }

    return !ContainsNoCase(subKey, "\\Card") &&
           !ContainsNoCase(subKey, "\\KeyConfig");
}

static bool IsMMX3CardKeyPath(LPCSTR subKey)
{
    return IsMMX3BaseKeyPath(subKey) &&
           ContainsNoCase(subKey, "\\Card");
}

static bool IsMMX3KeyConfigPath(LPCSTR subKey)
{
    return IsMMX3BaseKeyPath(subKey) &&
           ContainsNoCase(subKey, "\\KeyConfig");
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
        LogLine("Read binary: %s not found, using default size=%lu",
                path,
                (unsigned long)defaultSize);

        return ReturnRegBinary(defaultData, defaultSize, lpType, lpData, lpcbData);
    }

    BYTE temp[512];
    memset(temp, 0, sizeof(temp));

    size_t n = fread(temp, 1, sizeof(temp), fp);
    fclose(fp);

    LogLine("Read binary: %s size=%lu", path, (unsigned long)n);

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
        LogLine("Write binary failed: %s", path);
        return ERROR_ACCESS_DENIED;
    }

    size_t written = fwrite(data, 1, size, fp);
    fclose(fp);

    LogLine("Write binary: %s size=%lu written=%lu",
            path,
            (unsigned long)size,
            (unsigned long)written);

    return written == size ? ERROR_SUCCESS : ERROR_WRITE_FAULT;
}

// Registry hooks

static LONG WINAPI MyRegOpenKeyExA(
    HKEY hKey,
    LPCSTR lpSubKey,
    DWORD ulOptions,
    REGSAM samDesired,
    PHKEY phkResult)
{
    if (phkResult && IsMMX3CardKeyPath(lpSubKey)) {
        *phkResult = FAKE_HKEY_MMX3_CARD;
        LogLine("RegOpenKeyExA fake Card key: %s", lpSubKey);
        return ERROR_SUCCESS;
    }

    if (phkResult && IsMMX3KeyConfigPath(lpSubKey)) {
        *phkResult = FAKE_HKEY_MMX3_KEYCFG;
        LogLine("RegOpenKeyExA fake KeyConfig key: %s", lpSubKey);
        return ERROR_SUCCESS;
    }

    if (phkResult && IsMMX3RootKeyPath(lpSubKey)) {
        *phkResult = FAKE_HKEY_MMX3;
        LogLine("RegOpenKeyExA fake Root key: %s", lpSubKey);
        return ERROR_SUCCESS;
    }

    return g_realRegOpenKeyExA
        ? g_realRegOpenKeyExA(hKey, lpSubKey, ulOptions, samDesired, phkResult)
        : ERROR_FILE_NOT_FOUND;
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
        LogLine("RegCreateKeyExA fake Card key: %s", lpSubKey);
        return ERROR_SUCCESS;
    }

    if (phkResult && IsMMX3KeyConfigPath(lpSubKey)) {
        *phkResult = FAKE_HKEY_MMX3_KEYCFG;
        if (lpdwDisposition) {
            *lpdwDisposition = REG_OPENED_EXISTING_KEY;
        }
        LogLine("RegCreateKeyExA fake KeyConfig key: %s", lpSubKey);
        return ERROR_SUCCESS;
    }

    if (phkResult && IsMMX3RootKeyPath(lpSubKey)) {
        *phkResult = FAKE_HKEY_MMX3;
        if (lpdwDisposition) {
            *lpdwDisposition = REG_OPENED_EXISTING_KEY;
        }
        LogLine("RegCreateKeyExA fake Root key: %s", lpSubKey);
        return ERROR_SUCCESS;
    }

    return g_realRegCreateKeyExA
        ? g_realRegCreateKeyExA(
            hKey,
            lpSubKey,
            Reserved,
            lpClass,
            dwOptions,
            samDesired,
            lpSecurityAttributes,
            phkResult,
            lpdwDisposition)
        : ERROR_FILE_NOT_FOUND;
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
            LogLine("RegQuery Root %s -> %s", lpValueName, g_gameDir);
            return ReturnRegSz(g_gameDir, lpType, lpData, lpcbData);
        }

        if (IsValue(lpValueName, "Inst Program") ||
            IsValue(lpValueName, "Inst Resource") ||
            IsValue(lpValueName, "Inst Movie") ||
            IsValue(lpValueName, "Inst BGM") ||
            IsValue(lpValueName, "DoubleView") ||
            IsValue(lpValueName, "Easy Mode")) {
            LogLine("RegQuery Root %s -> True", lpValueName);
            return ReturnRegSz("True", lpType, lpData, lpcbData);
        }

        if (IsValue(lpValueName, "FullScreen")) {
            LogLine("RegQuery Root FullScreen -> False");
            return ReturnRegSz("False", lpType, lpData, lpcbData);
        }

        if (IsValue(lpValueName, "Screen Mode")) {
            LogLine("RegQuery Root Screen Mode -> 640,480,8");
            return ReturnRegSz("640,480,8", lpType, lpData, lpcbData);
        }

        LogLine("RegQuery Root unknown value: %s", lpValueName ? lpValueName : "(null)");
        return ERROR_FILE_NOT_FOUND;
    }

    if (hKey == FAKE_HKEY_MMX3_CARD) {
        if (IsValue(lpValueName, "Password")) {
            LogLine("RegQuery Card Password");
            return ReturnBinaryFileOrDefault(
                g_passwordPath,
                kDefaultPassword,
                sizeof(kDefaultPassword),
                lpType,
                lpData,
                lpcbData);
        }
        LogLine("RegQuery Card unknown value: %s", lpValueName ? lpValueName : "(null)");
        return ERROR_FILE_NOT_FOUND;
    }

    if (hKey == FAKE_HKEY_MMX3_KEYCFG) {
        if (IsValue(lpValueName, "GamePad")) {
            LogLine("RegQuery KeyConfig GamePad");
            return ReturnBinaryFileOrDefault(
                g_keyGamePadPath,
                kDefaultGamePad,
                sizeof(kDefaultGamePad),
                lpType,
                lpData,
                lpcbData);
        }

        if (IsValue(lpValueName, "Keyboard")) {
            LogLine("RegQuery KeyConfig Keyboard");
            return ReturnBinaryFileOrDefault(
                g_keyKeyboardPath,
                kDefaultKeyboard,
                sizeof(kDefaultKeyboard),
                lpType,
                lpData,
                lpcbData);
        }

        if (IsValue(lpValueName, "SideWinder")) {
            LogLine("RegQuery KeyConfig SideWinder");
            return ReturnBinaryFileOrDefault(
                g_keySideWinderPath,
                kDefaultSideWinder,
                sizeof(kDefaultSideWinder),
                lpType,
                lpData,
                lpcbData);
        }

        LogLine("RegQuery KeyConfig unknown value: %s", lpValueName ? lpValueName : "(null)");
        return ERROR_FILE_NOT_FOUND;
    }

    return g_realRegQueryValueExA
        ? g_realRegQueryValueExA(
            hKey,
            lpValueName,
            lpReserved,
            lpType,
            lpData,
            lpcbData)
        : ERROR_FILE_NOT_FOUND;
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
            LogLine("RegSet Card Password size=%lu", (unsigned long)cbData);
            return WriteBinaryFile(g_passwordPath, lpData, 0x80);
        }
        LogLine("RegSet Card ignored value=%s type=%lu size=%lu",
                lpValueName ? lpValueName : "(null)",
                (unsigned long)dwType,
                (unsigned long)cbData);
        return ERROR_SUCCESS;
    }

    if (hKey == FAKE_HKEY_MMX3_KEYCFG) {
        if (IsValue(lpValueName, "GamePad") &&
            dwType == REG_BINARY &&
            lpData &&
            cbData > 0) {
            LogLine("RegSet KeyConfig GamePad size=%lu", (unsigned long)cbData);
            return WriteBinaryFile(g_keyGamePadPath, lpData, cbData);
        }

        if (IsValue(lpValueName, "Keyboard") &&
            dwType == REG_BINARY &&
            lpData &&
            cbData > 0) {
            LogLine("RegSet KeyConfig Keyboard size=%lu", (unsigned long)cbData);
            return WriteBinaryFile(g_keyKeyboardPath, lpData, cbData);
        }

        if (IsValue(lpValueName, "SideWinder") &&
            dwType == REG_BINARY &&
            lpData &&
            cbData > 0) {
            LogLine("RegSet KeyConfig SideWinder size=%lu", (unsigned long)cbData);
            return WriteBinaryFile(g_keySideWinderPath, lpData, cbData);
        }

        LogLine("RegSet KeyConfig ignored value=%s type=%lu size=%lu",
                lpValueName ? lpValueName : "(null)",
                (unsigned long)dwType,
                (unsigned long)cbData);
        return ERROR_SUCCESS;
    }

    if (hKey == FAKE_HKEY_MMX3) {
        LogLine("RegSet Root ignored value=%s type=%lu size=%lu",
                lpValueName ? lpValueName : "(null)",
                (unsigned long)dwType,
                (unsigned long)cbData);
        return ERROR_SUCCESS;
    }

    return g_realRegSetValueExA
        ? g_realRegSetValueExA(
            hKey,
            lpValueName,
            Reserved,
            dwType,
            lpData,
            cbData)
        : ERROR_SUCCESS;
}

static LONG WINAPI MyRegCloseKey(HKEY hKey)
{
    if (hKey == FAKE_HKEY_MMX3 ||
        hKey == FAKE_HKEY_MMX3_CARD ||
        hKey == FAKE_HKEY_MMX3_KEYCFG) {
        LogLine("RegCloseKey fake handle=%p", hKey);
        return ERROR_SUCCESS;
    }

    return g_realRegCloseKey
        ? g_realRegCloseKey(hKey)
        : ERROR_SUCCESS;
}

void InstallRegistryHooks(HMODULE exe)
{
    LogLine("InstallRegistryHooks");

    PatchIAT(
        exe,
        "ADVAPI32.DLL",
        "RegOpenKeyExA",
        (void *)MyRegOpenKeyExA,
        (void **)&g_realRegOpenKeyExA);

    PatchIAT(
        exe,
        "ADVAPI32.DLL",
        "RegCreateKeyExA",
        (void *)MyRegCreateKeyExA,
        (void **)&g_realRegCreateKeyExA);

    PatchIAT(
        exe,
        "ADVAPI32.DLL",
        "RegQueryValueExA",
        (void *)MyRegQueryValueExA,
        (void **)&g_realRegQueryValueExA);

    PatchIAT(
        exe,
        "ADVAPI32.DLL",
        "RegSetValueExA",
        (void *)MyRegSetValueExA,
        (void **)&g_realRegSetValueExA);

    PatchIAT(
        exe,
        "ADVAPI32.DLL",
        "RegCloseKey",
        (void *)MyRegCloseKey,
        (void **)&g_realRegCloseKey);
}
