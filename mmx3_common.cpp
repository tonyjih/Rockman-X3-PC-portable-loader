#include "mmx3_common.h"

#include <stdio.h>
#include <string.h>
#include <stdarg.h>

char g_gameDir[MAX_PATH];
char g_gameDriveRoot[4];

char g_passwordPath[MAX_PATH];
char g_keyGamePadPath[MAX_PATH];
char g_keyKeyboardPath[MAX_PATH];
char g_keySideWinderPath[MAX_PATH];
char g_logPath[MAX_PATH];

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

    lstrcpynA(g_passwordPath, g_gameDir, MAX_PATH);
    lstrcatA(g_passwordPath, "MMX3.sav");

    lstrcpynA(g_keyGamePadPath, g_gameDir, MAX_PATH);
    lstrcatA(g_keyGamePadPath, "KeyConfig_GamePad.bin");

    lstrcpynA(g_keyKeyboardPath, g_gameDir, MAX_PATH);
    lstrcatA(g_keyKeyboardPath, "KeyConfig_Keyboard.bin");

    lstrcpynA(g_keySideWinderPath, g_gameDir, MAX_PATH);
    lstrcatA(g_keySideWinderPath, "KeyConfig_SideWinder.bin");

    lstrcpynA(g_logPath, g_gameDir, MAX_PATH);
    lstrcatA(g_logPath, "mmx3_portable.log");

#if MMX3_ENABLE_LOG
    FILE *fp = fopen(g_logPath, "w");
    if (fp) {
        fprintf(fp, "MMX3 portable loader log start\n");
        fclose(fp);
    }

    LogLine("GameDir=%s", g_gameDir);
    LogLine("GameDriveRoot=%s", g_gameDriveRoot);
    LogLine("PasswordPath=%s", g_passwordPath);
    LogLine("KeyGamePadPath=%s", g_keyGamePadPath);
    LogLine("KeyKeyboardPath=%s", g_keyKeyboardPath);
    LogLine("KeySideWinderPath=%s", g_keySideWinderPath);
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
