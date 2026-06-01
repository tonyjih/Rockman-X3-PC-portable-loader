#include "mmx3_common.h"

#include <stdint.h>
#include <string.h>

// ============================================================
// Bugfix switches
// ============================================================

#ifndef MMX3_ENABLE_PROJECTILE_STATE_LOG
#define MMX3_ENABLE_PROJECTILE_STATE_LOG MMX3_ENABLE_LOG
#endif

#ifndef MMX3_ENABLE_MISSING_MOVE_OBJECT_ARGUMENT_FIX
#define MMX3_ENABLE_MISSING_MOVE_OBJECT_ARGUMENT_FIX 1
#endif

// ============================================================
// Small x86 code emitter helpers
// ============================================================

static void EmitRel32(BYTE *at, BYTE *target)
{
    intptr_t rel = target - (at + 4);
    *(int32_t *)at = (int32_t)rel;
}

static BYTE *EmitJmpRel32(BYTE *p, BYTE *target)
{
    *p++ = 0xE9;
    EmitRel32(p, target);
    p += 4;
    return p;
}

// ============================================================
// EXE build detection
// ============================================================

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

static bool IsMegaManX3Build(HMODULE exe)
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

// ============================================================
// Projectile state logger for FUN_00476E46
// ============================================================
//
// Debug-only instrumentation.
// Enabled when MMX3_ENABLE_PROJECTILE_STATE_LOG is non-zero.
// By default, this follows MMX3_ENABLE_LOG.
//

#if MMX3_ENABLE_PROJECTILE_STATE_LOG

static LONG g_projectileStateLogCount = 0;

extern "C" __declspec(noinline)
void __stdcall LogProjectileState(DWORD parent)
{
    LONG n = InterlockedIncrement(&g_projectileStateLogCount);

    if (n > 128) {
        return;
    }

    if (parent < 0x10000) {
        LogLine("ProjectileState invalid parent=%08lX", (unsigned long)parent);
        return;
    }

    DWORD child = parent + 0x40;

    BYTE p01 = 0;
    BYTE p10 = 0;
    short p1e = 0;
    short p20 = 0;

    BYTE c01 = 0;
    BYTE c03 = 0;
    BYTE c06 = 0;
    BYTE c10 = 0;
    short c1e = 0;
    short c20 = 0;

    WORD c04w = 0;
    WORD c06w = 0;
    WORD c08w = 0;
    WORD c0aw = 0;

    __try {
        p01 = *(BYTE *)(parent + 0x01);
        p10 = *(BYTE *)(parent + 0x10);
        p1e = *(short *)(parent + 0x1E);
        p20 = *(short *)(parent + 0x20);

        c01 = *(BYTE *)(child + 0x01);
        c03 = *(BYTE *)(child + 0x03);
        c06 = *(BYTE *)(child + 0x06);
        c10 = *(BYTE *)(child + 0x10);
        c1e = *(short *)(child + 0x1E);
        c20 = *(short *)(child + 0x20);

        c04w = *(WORD *)(child + 0x04);
        c06w = *(WORD *)(child + 0x06);
        c08w = *(WORD *)(child + 0x08);
        c0aw = *(WORD *)(child + 0x0A);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        LogLine(
            "ProjectileState read failed parent=%08lX child=%08lX",
            (unsigned long)parent,
            (unsigned long)child);
        return;
    }

    LogLine(
        "ProjectileState #%ld p=%08lX child=%08lX "
        "p01=%02X p10=%02X p1E=%d p20=%d "
        "c01=%02X c03=%02X c06=%02X c10=%02X c1E=%d c20=%d "
        "c04w=%04X c06w=%04X c08w=%04X c0Aw=%04X",
        (long)n,
        (unsigned long)parent,
        (unsigned long)child,
        (unsigned int)p01,
        (unsigned int)p10,
        (int)p1e,
        (int)p20,
        (unsigned int)c01,
        (unsigned int)c03,
        (unsigned int)c06,
        (unsigned int)c10,
        (int)c1e,
        (int)c20,
        (unsigned int)c04w,
        (unsigned int)c06w,
        (unsigned int)c08w,
        (unsigned int)c0aw);
}

static void PatchProjectileStateLogger(HMODULE exe)
{
    if (!exe) {
        LogLine("PatchProjectileStateLogger skipped: exe is null");
        return;
    }

    BYTE *base = (BYTE *)exe;

    BYTE *target = base + 0x00076E46; // US MEGAMAN X3 only: Ghidra VA 00476E46
    BYTE *resume = base + 0x00076E52; // Continue at ADD EAX, 0x40

    BYTE *gate = (BYTE *)VirtualAlloc(
        NULL,
        128,
        MEM_COMMIT | MEM_RESERVE,
        PAGE_EXECUTE_READWRITE);

    if (!gate) {
        LogLine("PatchProjectileStateLogger VirtualAlloc failed");
        return;
    }

    BYTE *p = gate;

    // Replay overwritten prologue:
    //
    // 00476E46 push ebp
    // 00476E47 mov  ebp, esp
    // 00476E49 sub  esp, 8
    // 00476E4C push ebx
    // 00476E4D push esi
    // 00476E4E push edi
    // 00476E4F mov  eax, [ebp+8]
    *p++ = 0x55;
    *p++ = 0x8B; *p++ = 0xEC;
    *p++ = 0x83; *p++ = 0xEC; *p++ = 0x08;
    *p++ = 0x53;
    *p++ = 0x56;
    *p++ = 0x57;
    *p++ = 0x8B; *p++ = 0x45; *p++ = 0x08;

    // push eax
    // argument: parent = [ebp+8]
    *p++ = 0x50;

    // call LogProjectileState
    *p++ = 0xE8;
    EmitRel32(p, (BYTE *)&LogProjectileState);
    p += 4;

    // Reload eax because the call may clobber it.
    // mov eax, [ebp+8]
    *p++ = 0x8B;
    *p++ = 0x45;
    *p++ = 0x08;

    // jmp 00476E52
    p = EmitJmpRel32(p, resume);

    LogLine(
        "PatchProjectileStateLogger gate=%p target=%p resume=%p",
        gate,
        target,
        resume);

    BYTE patch[12];
    memset(patch, 0x90, sizeof(patch));

    patch[0] = 0xE9;
    *(int32_t *)&patch[1] = (int32_t)(gate - (target + 5));

    PatchMemory(target, patch, sizeof(patch));
}

#else

static void PatchProjectileStateLogger(HMODULE)
{
    // Debug-only logger disabled.
}

#endif

// ============================================================
// Fix: missing argument before CALL FUN_0042BCC7 at 00476E60
// ============================================================
//
// US / MEGAMAN X3 only.
//
// Root cause:
//
//   FUN_00476E46 calls FUN_0042BCC7 without pushing its argument.
//
// Original:
//
//   00476E4F  mov eax, [ebp+8]
//   00476E52  add eax, 40h
//   00476E55  mov [ebp-8], eax
//   00476E58  mov eax, [004F16DC]
//   00476E5D  mov [ebp-4], eax
//   00476E60  call 0042BCC7          ; BUG: missing argument
//   00476E65  mov eax, [ebp-8]
//
// Correct behavior found by tracing:
//
//   The movement helper must receive the parent projectile object.
//
// Patch:
//
//   Replace the 5-byte CALL at 00476E60 with JMP gate.
//
//   gate:
//     push dword ptr [ebp+8]
//     call 0042BCC7
//     add esp, 4
//     jmp 00476E65
//

static void PatchMissingMoveObjectArgument(HMODULE exe)
{
#if MMX3_ENABLE_MISSING_MOVE_OBJECT_ARGUMENT_FIX
    if (!exe) {
        LogLine("PatchMissingMoveObjectArgument skipped: exe is null");
        return;
    }

    BYTE *base = (BYTE *)exe;

    BYTE *target = base + 0x00076E60; // US MEGAMAN X3 only: Ghidra VA 00476E60
    BYTE *callee = base + 0x0002BCC7; // US MEGAMAN X3 only: Ghidra VA 0042BCC7
    BYTE *resume = base + 0x00076E65; // After original CALL

    BYTE *gate = (BYTE *)VirtualAlloc(
        NULL,
        64,
        MEM_COMMIT | MEM_RESERVE,
        PAGE_EXECUTE_READWRITE);

    if (!gate) {
        LogLine("PatchMissingMoveObjectArgument VirtualAlloc failed");
        return;
    }

    BYTE *p = gate;

    // push dword ptr [ebp+0x8]
    // parent projectile object = param_1
    *p++ = 0xFF;
    *p++ = 0x75;
    *p++ = 0x08;

    // call FUN_0042BCC7
    *p++ = 0xE8;
    EmitRel32(p, callee);
    p += 4;

    // add esp, 4
    *p++ = 0x83;
    *p++ = 0xC4;
    *p++ = 0x04;

    // jmp 00476E65
    p = EmitJmpRel32(p, resume);

    LogLine(
        "PatchMissingMoveObjectArgument gate=%p target=%p callee=%p resume=%p",
        gate,
        target,
        callee,
        resume);

    // Replace original CALL with JMP gate.
    BYTE patch[5];
    patch[0] = 0xE9;
    *(int32_t *)&patch[1] = (int32_t)(gate - (target + 5));

    PatchMemory(target, patch, sizeof(patch));
#else
    LogLine("PatchMissingMoveObjectArgument disabled");
#endif
}

// ============================================================
// Installer
// ============================================================

void InstallBugFixes(HMODULE exe)
{
    LogLine("InstallBugFixes");

    if (!IsMegaManX3Build(exe)) {
        LogLine("InstallBugFixes skipped: not MEGAMAN X3 build");
        return;
    }

    LogLine("InstallBugFixes applying MEGAMAN X3 gameplay fixes");

    // Debug/log build only.
    PatchProjectileStateLogger(exe);

    // Minimal root-cause fix for US / MEGAMAN X3.
    if (g_patchConfig.bossProjectileFix) {
        PatchMissingMoveObjectArgument(exe);
    } else {
        LogLine("BossProjectileFix skipped: disabled by MMX3.conf");
    }
}