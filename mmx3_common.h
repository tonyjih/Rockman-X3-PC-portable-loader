#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#ifndef MMX3_ENABLE_LOG
#define MMX3_ENABLE_LOG 1
#endif


extern char g_gameDir[MAX_PATH];
extern char g_gameDriveRoot[4];

extern char g_passwordPath[MAX_PATH];
extern char g_configPath[MAX_PATH];
extern char g_logPath[MAX_PATH];

void BuildPaths();
void LogLine(const char *fmt, ...);

bool PatchMemory(
    void *address,
    const BYTE *patch,
    SIZE_T patchSize);

bool PatchIAT(
    HMODULE module,
    const char *dllName,
    const char *funcName,
    void *newFunc,
    void **oldFunc);

void InstallRegistryHooks(HMODULE exe);
void InstallCdHooks(HMODULE exe);
void InstallBugFixes(HMODULE exe);
void InstallTimingHooks(HMODULE exe);
