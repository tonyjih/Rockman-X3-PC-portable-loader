#include "mmx3_common.h"

#include <stdio.h>
#include <string.h>
#include <strsafe.h>
#include <stdarg.h>

char g_gameDir[MAX_PATH];
char g_gameDriveRoot[4];

char g_passwordPath[MAX_PATH];
char g_configPath[MAX_PATH];
char g_logPath[MAX_PATH];


MMX3PatchConfig g_patchConfig;

const char *MMX3BoolText(BOOL value)
{
    return value ? "True" : "False";
}

static BOOL ParseConfigBoolText(const char *text, BOOL defaultValue)
{
    if (!text || !text[0]) {
        return defaultValue;
    }

    if (_stricmp(text, "1") == 0 ||
        _stricmp(text, "true") == 0 ||
        _stricmp(text, "yes") == 0 ||
        _stricmp(text, "on") == 0 ||
        _stricmp(text, "enable") == 0 ||
        _stricmp(text, "enabled") == 0) {
        return TRUE;
    }

    if (_stricmp(text, "0") == 0 ||
        _stricmp(text, "false") == 0 ||
        _stricmp(text, "no") == 0 ||
        _stricmp(text, "off") == 0 ||
        _stricmp(text, "disable") == 0 ||
        _stricmp(text, "disabled") == 0) {
        return FALSE;
    }

    return defaultValue;
}

void LoadPatchConfigDefaults()
{
    g_patchConfig.bossProjectileFix = TRUE;
    g_patchConfig.fractional60FpsTimer = TRUE;
    g_patchConfig.normalizeScreenMode = TRUE;
    g_patchConfig.zeroValuableItemPickupFix = TRUE;
}

BOOL GetPortableConfigBool(const char *section, const char *key, BOOL defaultValue)
{
    if (!g_configPath[0]) {
        return defaultValue;
    }

    char value[64];
    value[0] = 0;

    GetPrivateProfileStringA(
        section,
        key,
        MMX3BoolText(defaultValue),
        value,
        sizeof(value),
        g_configPath);

    return ParseConfigBoolText(value, defaultValue);
}

static void EnsurePortableConfigBool(const char *section, const char *key, BOOL defaultValue)
{
    if (!g_configPath[0]) {
        return;
    }

    char value[64];
    value[0] = 0;

    GetPrivateProfileStringA(section, key, "", value, sizeof(value), g_configPath);

    if (value[0] == 0) {
        WritePrivateProfileStringA(section, key, MMX3BoolText(defaultValue), g_configPath);
    }
}

void LoadPatchConfigFromPortableConfig()
{
    LoadPatchConfigDefaults();

    EnsurePortableConfigBool("Patches", "BossProjectileFix", g_patchConfig.bossProjectileFix);
    EnsurePortableConfigBool("Patches", "Fractional60FpsTimer", g_patchConfig.fractional60FpsTimer);
    EnsurePortableConfigBool("Patches", "NormalizeScreenMode", g_patchConfig.normalizeScreenMode);
    EnsurePortableConfigBool("Patches", "ZeroValuableItemPickupFix", g_patchConfig.zeroValuableItemPickupFix);

    g_patchConfig.bossProjectileFix =
        GetPortableConfigBool("Patches", "BossProjectileFix", g_patchConfig.bossProjectileFix);
    g_patchConfig.fractional60FpsTimer =
        GetPortableConfigBool("Patches", "Fractional60FpsTimer", g_patchConfig.fractional60FpsTimer);
    g_patchConfig.normalizeScreenMode =
        GetPortableConfigBool("Patches", "NormalizeScreenMode", g_patchConfig.normalizeScreenMode);

    g_patchConfig.zeroValuableItemPickupFix =
        GetPortableConfigBool("Patches", "ZeroValuableItemPickupFix", g_patchConfig.zeroValuableItemPickupFix);

    LogLine(
        "PatchConfig: BossProjectileFix=%s Fractional60FpsTimer=%s NormalizeScreenMode=%s ZeroValuableItemPickupFix=%s",
        MMX3BoolText(g_patchConfig.bossProjectileFix),
        MMX3BoolText(g_patchConfig.fractional60FpsTimer),
        MMX3BoolText(g_patchConfig.normalizeScreenMode),
        MMX3BoolText(g_patchConfig.zeroValuableItemPickupFix));
}

void LogLine(const char *fmt, ...)
{
#if MMX3_ENABLE_LOG
    if (!g_logPath[0]) {
        return;
    }

    FILE *fp = fopen(g_logPath, "a");
    if (!fp) {
        return;
    }

    SYSTEMTIME st;
    GetLocalTime(&st);

    fprintf(
        fp,
        "[%04u-%02u-%02u %02u:%02u:%02u.%03u] ",
        st.wYear,
        st.wMonth,
        st.wDay,
        st.wHour,
        st.wMinute,
        st.wSecond,
        st.wMilliseconds);

    va_list args;
    va_start(args, fmt);
    vfprintf(fp, fmt, args);
    va_end(args);

    fprintf(fp, "\n");
    fclose(fp);
#else
    (void)fmt;
#endif
}

static bool MemoryContains(const BYTE *base, SIZE_T size, const char *needle)
{
    if (!base || !needle) {
        return false;
    }

    SIZE_T needleLen = strlen(needle);

    if (needleLen == 0 || size < needleLen) {
        return false;
    }

    for (SIZE_T i = 0; i <= size - needleLen; i++) {
        if (memcmp(base + i, needle, needleLen) == 0) {
            return true;
        }
    }

    return false;
}

static bool ExeImageContainsString(HMODULE exe, const char *needle)
{
    if (!exe || !needle) {
        return false;
    }

    BYTE *base = (BYTE *)exe;

    __try {
        IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;

        if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
            return false;
        }

        IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);

        if (nt->Signature != IMAGE_NT_SIGNATURE) {
            return false;
        }

        SIZE_T imageSize = nt->OptionalHeader.SizeOfImage;

        return MemoryContains(base, imageSize, needle);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool IsMegaManX3Build(HMODULE exe)
{
    bool hasMegaMan = ExeImageContainsString(exe, "MEGAMAN X3");
    bool hasRockMan = ExeImageContainsString(exe, "ROCKMAN X3");

    LogLine(
        "Game build detection: MEGAMAN_X3=%d ROCKMAN_X3=%d",
        hasMegaMan ? 1 : 0,
        hasRockMan ? 1 : 0);

    // For now, gameplay bugfix addresses are confirmed only for the US / MEGAMAN X3 build.
    // The JP / ROCKMAN X3 build likely needs different RVAs.
    return hasMegaMan && !hasRockMan;
}

void BuildPaths()
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

    StringCchPrintfA(g_passwordPath, MAX_PATH, "%sMMX3.sav", g_gameDir);
    StringCchPrintfA(g_configPath, MAX_PATH, "%sMMX3.conf", g_gameDir);
    StringCchPrintfA(g_logPath, MAX_PATH, "%smmx3_portable.log", g_gameDir);

#if MMX3_ENABLE_LOG
    FILE *fp = fopen(g_logPath, "w");
    if (fp) {
        fprintf(fp, "MMX3 portable loader log start\n");
        fclose(fp);
    }

    LogLine("GameDir=%s", g_gameDir);
    LogLine("GameDriveRoot=%s", g_gameDriveRoot);
    LogLine("PasswordPath=%s", g_passwordPath);
    LogLine("ConfigPath=%s", g_configPath);
#endif
}

bool PatchMemory(void *address, const BYTE *patch, SIZE_T patchSize)
{
    DWORD oldProtect;

    if (!VirtualProtect(address, patchSize, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        LogLine(
            "PatchMemory VirtualProtect failed addr=%p size=%lu",
            address,
            (unsigned long)patchSize);
        return false;
    }

    memcpy(address, patch, patchSize);
    FlushInstructionCache(GetCurrentProcess(), address, patchSize);

    DWORD dummy;
    VirtualProtect(address, patchSize, oldProtect, &dummy);

    LogLine(
        "PatchMemory OK addr=%p size=%lu",
        address,
        (unsigned long)patchSize);

    return true;
}

bool PatchIAT(
    HMODULE module,
    const char *dllName,
    const char *funcName,
    void *newFunc,
    void **oldFunc)
{
    if (!module || !dllName || !funcName || !newFunc) {
        LogLine("PatchIAT invalid argument");
        return false;
    }

    BYTE *base = (BYTE *)module;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;

    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        LogLine("PatchIAT failed: invalid DOS header for %s!%s", dllName, funcName);
        return false;
    }

    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);

    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        LogLine("PatchIAT failed: invalid NT header for %s!%s", dllName, funcName);
        return false;
    }

    IMAGE_DATA_DIRECTORY importDir =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];

    if (!importDir.VirtualAddress) {
        LogLine("PatchIAT failed: no import dir for %s!%s", dllName, funcName);
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
                    LogLine("PatchIAT VirtualProtect failed: %s!%s", dllName, funcName);
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

                LogLine("PatchIAT OK: %s!%s", dllName, funcName);
                return true;
            }
        }
    }

    LogLine("PatchIAT target not found: %s!%s", dllName, funcName);
    return false;
}
