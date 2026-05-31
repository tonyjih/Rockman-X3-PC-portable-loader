#include "mmx3_common.h"

#include <mmsystem.h>
#include <stdio.h>

#define MMX3_ENABLE_MCI_DEVICE_OPEN_PATCH      1
#define MMX3_ENABLE_POST_THREAD_MESSAGE_LOG    1
#define MMX3_ENABLE_MCI_STATUS_HOOK            1

typedef UINT (WINAPI *GetDriveTypeA_t)(LPCSTR lpRootPathName);

typedef BOOL (WINAPI *PostThreadMessageA_t)(
    DWORD idThread,
    UINT Msg,
    WPARAM wParam,
    LPARAM lParam);

typedef MCIERROR (WINAPI *mciSendCommandA_t)(
    MCIDEVICEID mciId,
    UINT uMsg,
    DWORD_PTR dwParam1,
    DWORD_PTR dwParam2);

static GetDriveTypeA_t        g_realGetDriveTypeA      = NULL;
static PostThreadMessageA_t   g_realPostThreadMessageA = NULL;
static mciSendCommandA_t      g_realMciSendCommandA    = NULL;

static void PatchMciDeviceOpenCheck()
{
#if MMX3_ENABLE_MCI_DEVICE_OPEN_PATCH
    HMODULE exe = GetModuleHandleA(NULL);
    if (!exe) {
        return;
    }

    // Ghidra VA : 0x00403C00
    // ImageBase : 0x00400000
    // RVA       : 0x00003C00
    // Original  : return *(int *)(this + 0x14) != 0;
    // Patch     : mov eax, 1; ret
    BYTE *target = (BYTE *)exe + 0x00003C00;

    static const BYTE patch[] = {
        0xB8, 0x01, 0x00, 0x00, 0x00,
        0xC3
    };

    LogLine("Applying PatchMciDeviceOpenCheck at RVA 0x00003C00");
    PatchMemory(target, patch, sizeof(patch));
#else
    LogLine("PatchMciDeviceOpenCheck disabled");
#endif
}

static UINT WINAPI MyGetDriveTypeA(LPCSTR lpRootPathName)
{
    if (lpRootPathName && g_gameDriveRoot[0]) {
        if (lstrcmpiA(lpRootPathName, g_gameDriveRoot) == 0) {
            LogLine("GetDriveTypeA(%s) -> DRIVE_CDROM exact", lpRootPathName);
            return DRIVE_CDROM;
        }

        if (lpRootPathName[0] &&
            lpRootPathName[1] == ':' &&
            (lpRootPathName[0] == g_gameDriveRoot[0] ||
             lpRootPathName[0] == (char)(g_gameDriveRoot[0] + 32) ||
             lpRootPathName[0] == (char)(g_gameDriveRoot[0] - 32))) {
            LogLine("GetDriveTypeA(%s) -> DRIVE_CDROM loose", lpRootPathName);
            return DRIVE_CDROM;
        }
    }

    UINT result = g_realGetDriveTypeA
        ? g_realGetDriveTypeA(lpRootPathName)
        : DRIVE_UNKNOWN;

    if (lpRootPathName) {
        LogLine("GetDriveTypeA(%s) -> %u real", lpRootPathName, result);
    }

    return result;
}

static BOOL WINAPI MyPostThreadMessageA(
    DWORD idThread,
    UINT Msg,
    WPARAM wParam,
    LPARAM lParam)
{
#if MMX3_ENABLE_POST_THREAD_MESSAGE_LOG
    LogLine(
        "PostThreadMessageA thread=%lu msg=0x%X wParam=0x%p lParam=0x%p",
        (unsigned long)idThread,
        Msg,
        (void *)wParam,
        (void *)lParam);
#endif

    return g_realPostThreadMessageA
        ? g_realPostThreadMessageA(idThread, Msg, wParam, lParam)
        : FALSE;
}

static bool IsFakeMciDevice(MCIDEVICEID id)
{
    return id == 0 || id == 1;
}

static MCIERROR WINAPI MyMciSendCommandA(
    MCIDEVICEID mciId,
    UINT uMsg,
    DWORD_PTR dwParam1,
    DWORD_PTR dwParam2)
{
#if MMX3_ENABLE_MCI_STATUS_HOOK
    DWORD s0 = 0;
    DWORD s1 = 0;
    DWORD s2 = 0;
    DWORD s3 = 0;

    if (dwParam2 != 0) {
        __try {
            DWORD *p = (DWORD *)dwParam2;
            s0 = p[0];
            s1 = p[1];
            s2 = p[2];
            s3 = p[3];
        } __except(EXCEPTION_EXECUTE_HANDLER) {
        }
    }

    LogLine(
        "mciSendCommandA call: id=%lu msg=0x%X flags=0x%p param2=0x%p s=[0x%08lX,0x%08lX,0x%08lX,0x%08lX]",
        (unsigned long)mciId,
        uMsg,
        (void *)dwParam1,
        (void *)dwParam2,
        (unsigned long)s0,
        (unsigned long)s1,
        (unsigned long)s2,
        (unsigned long)s3);

    // 0x803 = MCI_OPEN
    if (uMsg == 0x803 && dwParam2 != 0) {
        __try {
            DWORD *open = (DWORD *)dwParam2;

            // MCI_OPEN_PARMSA:
            // open[0] = dwCallback
            // open[1] = wDeviceID
            // open[2] = lpstrDeviceType
            // open[3] = lpstrElementName
            open[1] = 1;

            LogLine(
                "mciSendCommandA fake MCI_OPEN: flags=0x%p deviceType=0x%08lX element=0x%08lX -> deviceId=1",
                (void *)dwParam1,
                (unsigned long)open[2],
                (unsigned long)open[3]);

            return 0;
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            LogLine("mciSendCommandA fake MCI_OPEN exception");
            return 0;
        }
    }

    if (IsFakeMciDevice(mciId)) {
        // 0x80D = MCI_SET
        if (uMsg == 0x80D) {
            LogLine("mciSendCommandA fake MCI_SET id=%lu -> success",
                    (unsigned long)mciId);
            return 0;
        }

        // 0x814 = MCI_STATUS
        if (uMsg == 0x814 && dwParam2 != 0) {
            __try {
                DWORD *status = (DWORD *)dwParam2;
                DWORD item = status[2];
                DWORD track = status[3];

                if (item == 1) {
                    if (track <= 1) {
                        status[1] = 1;
                    } else if (track == 2) {
                        status[1] = 0x2A01;
                    } else {
                        status[1] = 60000;
                    }

                    LogLine(
                        "mciSendCommandA fake MCI_STATUS id=%lu item=1 track=%lu -> %lu",
                        (unsigned long)mciId,
                        (unsigned long)track,
                        (unsigned long)status[1]);

                    return 0;
                }

                if (item == 2) {
                    status[1] = 0x2A01;

                    LogLine(
                        "mciSendCommandA fake MCI_STATUS id=%lu item=2 -> 0x2A01",
                        (unsigned long)mciId);

                    return 0;
                }

                if (item == 3) {
                    status[1] = 1;

                    LogLine(
                        "mciSendCommandA fake MCI_STATUS id=%lu item=3 -> track_count=1",
                        (unsigned long)mciId);

                    return 0;
                }

                status[1] = 1;

                LogLine(
                    "mciSendCommandA fake MCI_STATUS id=%lu item=%lu track=%lu -> 1",
                    (unsigned long)mciId,
                    (unsigned long)item,
                    (unsigned long)track);

                return 0;
            } __except(EXCEPTION_EXECUTE_HANDLER) {
                LogLine("mciSendCommandA fake MCI_STATUS exception");
                return 0;
            }
        }

        // 0x808 = MCI_PLAY
        if (uMsg == 0x808) {
            LogLine("mciSendCommandA fake MCI_PLAY id=%lu -> success",
                    (unsigned long)mciId);
            return 0;
        }

        // 0x804 = MCI_STOP
        if (uMsg == 0x804) {
            LogLine("mciSendCommandA fake MCI_STOP id=%lu -> success",
                    (unsigned long)mciId);
            return 0;
        }

        // 0x806 = MCI_CLOSE
        if (uMsg == 0x806) {
            LogLine("mciSendCommandA fake MCI_CLOSE id=%lu -> success",
                    (unsigned long)mciId);
            return 0;
        }

        LogLine(
            "mciSendCommandA fake unknown fake-device id=%lu msg=0x%X -> success",
            (unsigned long)mciId,
            uMsg);

        return 0;
    }
#endif

    MCIERROR result = g_realMciSendCommandA
        ? g_realMciSendCommandA(mciId, uMsg, dwParam1, dwParam2)
        : 0;

#if MMX3_ENABLE_MCI_STATUS_HOOK
    DWORD r0 = 0;
    DWORD r1 = 0;
    DWORD r2 = 0;
    DWORD r3 = 0;

    if (dwParam2 != 0) {
        __try {
            DWORD *p = (DWORD *)dwParam2;
            r0 = p[0];
            r1 = p[1];
            r2 = p[2];
            r3 = p[3];
        } __except(EXCEPTION_EXECUTE_HANDLER) {
        }
    }

    LogLine(
        "mciSendCommandA real result=0x%08lX post s=[0x%08lX,0x%08lX,0x%08lX,0x%08lX]",
        (unsigned long)result,
        (unsigned long)r0,
        (unsigned long)r1,
        (unsigned long)r2,
        (unsigned long)r3);
#endif

    return result;
}

void InstallCdHooks(HMODULE exe)
{
    LogLine("InstallCdHooks");

    PatchMciDeviceOpenCheck();

    PatchIAT(
        exe,
        "KERNEL32.DLL",
        "GetDriveTypeA",
        (void *)MyGetDriveTypeA,
        (void **)&g_realGetDriveTypeA);

    PatchIAT(
        exe,
        "USER32.DLL",
        "PostThreadMessageA",
        (void *)MyPostThreadMessageA,
        (void **)&g_realPostThreadMessageA);

    PatchIAT(
        exe,
        "WINMM.DLL",
        "mciSendCommandA",
        (void *)MyMciSendCommandA,
        (void **)&g_realMciSendCommandA);
}
