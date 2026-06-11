#include "mmx3_common.h"

#include <stdio.h>
#include <string.h>
#include <ctype.h>

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

#define MMX3_CONFIG_BINARY_MAX 512

struct Mmx3PortableConfig
{
    bool loaded;

    char cdDrive[64];
    char instProgram[16];
    char instResource[16];
    char instMovie[16];
    char instBgm[16];
    char doubleView[16];
    char easyMode[16];
    char fullScreen[16];
    char screenMode[64];

    char patchBossProjectileFix[16];
    char patchFractional60FpsTimer[16];
    char patchNormalizeScreenMode[16];
    char patchZeroValuableItemPickupFix[16];

    BYTE gamePad[MMX3_CONFIG_BINARY_MAX];
    DWORD gamePadSize;
    bool hasGamePad;

    BYTE keyboard[MMX3_CONFIG_BINARY_MAX];
    DWORD keyboardSize;
    bool hasKeyboard;

    BYTE sideWinder[MMX3_CONFIG_BINARY_MAX];
    DWORD sideWinderSize;
    bool hasSideWinder;
};

static Mmx3PortableConfig g_config;

// Helpers

static bool IsValue(LPCSTR a, LPCSTR b)
{
    return a && b && lstrcmpiA(a, b) == 0;
}

static BOOL ConfigTextToBool(const char *text, BOOL defaultValue)
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

static void SyncPatchConfigFromPortableStrings()
{
    g_patchConfig.bossProjectileFix = ConfigTextToBool(
        g_config.patchBossProjectileFix,
        g_patchConfig.bossProjectileFix);
    g_patchConfig.fractional60FpsTimer = ConfigTextToBool(
        g_config.patchFractional60FpsTimer,
        g_patchConfig.fractional60FpsTimer);
    g_patchConfig.normalizeScreenMode = ConfigTextToBool(
        g_config.patchNormalizeScreenMode,
        g_patchConfig.normalizeScreenMode);
    g_patchConfig.zeroValuableItemPickupFix = ConfigTextToBool(
        g_config.patchZeroValuableItemPickupFix,
        g_patchConfig.zeroValuableItemPickupFix);
}

static void NormalizeScreenModeValue(char *value, size_t valueSize)
{
    if (!g_patchConfig.normalizeScreenMode || !value || valueSize == 0) {
        return;
    }

    int width = 0;
    int height = 0;
    int bpp = 0;

    if (sscanf(value, "%d,%d,%d", &width, &height, &bpp) != 3) {
        CopyMemory(value, "640,480,32", 11);
        if (valueSize > 11) {
            value[11] = '\0';
        } else {
            value[valueSize - 1] = '\0';
        }
        LogLine("NormalizeScreenMode: invalid value -> 640,480,32");
        return;
    }

    if (width == 640 && height == 480 && bpp == 8) {
        CopyMemory(value, "640,480,32", 11);
        if (valueSize > 11) {
            value[11] = '\0';
        } else {
            value[valueSize - 1] = '\0';
        }
        LogLine("NormalizeScreenMode: 640,480,8 -> 640,480,32");
    }
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

static char *Trim(char *s)
{
    if (!s) {
        return s;
    }

    while (*s && isspace((unsigned char)*s)) {
        s++;
    }

    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) {
        end--;
    }
    *end = '\0';

    return s;
}

static void StripLineEnding(char *s)
{
    if (!s) {
        return;
    }

    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\r' || s[n - 1] == '\n')) {
        s[n - 1] = '\0';
        n--;
    }
}

static int HexNibble(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static bool DecodeHex(const char *text, BYTE *out, DWORD outMax, DWORD *outSize)
{
    if (outSize) {
        *outSize = 0;
    }

    if (!text || !out || !outSize) {
        return false;
    }

    DWORD count = 0;
    int high = -1;

    for (const char *p = text; *p; p++) {
        if (isspace((unsigned char)*p) || *p == ',' || *p == '-' || *p == '_') {
            continue;
        }

        int v = HexNibble(*p);
        if (v < 0) {
            return false;
        }

        if (high < 0) {
            high = v;
        } else {
            if (count >= outMax) {
                return false;
            }
            out[count++] = (BYTE)((high << 4) | v);
            high = -1;
        }
    }

    if (high >= 0) {
        return false;
    }

    *outSize = count;
    return true;
}

static void WriteHex(FILE *fp, const char *name, const BYTE *data, DWORD size)
{
    fprintf(fp, "%s=", name);
    for (DWORD i = 0; i < size; i++) {
        fprintf(fp, "%02X", data[i]);
    }
    fprintf(fp, "\n");
}

static void CopyString(char *dst, size_t dstSize, const char *src)
{
    if (!dst || dstSize == 0) {
        return;
    }

    if (!src) {
        src = "";
    }

    lstrcpynA(dst, src, (int)dstSize);
}

static void CopyRegSz(char *dst, size_t dstSize, const BYTE *data, DWORD dataSize)
{
    if (!dst || dstSize == 0) {
        return;
    }

    dst[0] = '\0';

    if (!data || dataSize == 0) {
        return;
    }

    DWORD copySize = dataSize;
    if (copySize >= dstSize) {
        copySize = (DWORD)dstSize - 1;
    }

    memcpy(dst, data, copySize);
    dst[copySize] = '\0';
    dst[dstSize - 1] = '\0';
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

    BYTE temp[MMX3_CONFIG_BINARY_MAX];
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

static void InitConfigDefaults()
{
    memset(&g_config, 0, sizeof(g_config));

    CopyString(g_config.instProgram, sizeof(g_config.instProgram), "True");
    CopyString(g_config.instResource, sizeof(g_config.instResource), "True");
    CopyString(g_config.instMovie, sizeof(g_config.instMovie), "True");
    CopyString(g_config.instBgm, sizeof(g_config.instBgm), "True");
    CopyString(g_config.doubleView, sizeof(g_config.doubleView), "True");
    CopyString(g_config.easyMode, sizeof(g_config.easyMode), "True");
    CopyString(g_config.fullScreen, sizeof(g_config.fullScreen), "False");
    CopyString(
        g_config.screenMode,
        sizeof(g_config.screenMode),
        g_patchConfig.normalizeScreenMode ? "640,480,32" : "640,480,8");
    CopyString(g_config.patchBossProjectileFix, sizeof(g_config.patchBossProjectileFix), MMX3BoolText(g_patchConfig.bossProjectileFix));
    CopyString(g_config.patchFractional60FpsTimer, sizeof(g_config.patchFractional60FpsTimer), MMX3BoolText(g_patchConfig.fractional60FpsTimer));
    CopyString(g_config.patchNormalizeScreenMode, sizeof(g_config.patchNormalizeScreenMode), MMX3BoolText(g_patchConfig.normalizeScreenMode));
    CopyString(g_config.patchZeroValuableItemPickupFix, sizeof(g_config.patchZeroValuableItemPickupFix), MMX3BoolText(g_patchConfig.zeroValuableItemPickupFix));
}

static void LoadPortableConfig()
{
    if (g_config.loaded) {
        return;
    }

    InitConfigDefaults();
    g_config.loaded = true;

    FILE *fp = fopen(g_configPath, "r");
    if (!fp) {
        LogLine("Portable config not found: %s", g_configPath);
        return;
    }

    LogLine("Portable config load: %s", g_configPath);

    char section[32] = "";
    char line[2048];

    while (fgets(line, sizeof(line), fp)) {
        StripLineEnding(line);
        char *s = Trim(line);

        if (!s[0] || s[0] == '#' || s[0] == ';') {
            continue;
        }

        if (s[0] == '[') {
            char *end = strchr(s, ']');
            if (end) {
                *end = '\0';
                CopyString(section, sizeof(section), Trim(s + 1));
            }
            continue;
        }

        char *eq = strchr(s, '=');
        if (!eq) {
            continue;
        }

        *eq = '\0';
        char *key = Trim(s);
        char *value = Trim(eq + 1);

        if (lstrcmpiA(section, "Root") == 0) {
            if (IsValue(key, "CD Drive")) {
                CopyString(g_config.cdDrive, sizeof(g_config.cdDrive), value);
            } else if (IsValue(key, "Inst Program")) {
                CopyString(g_config.instProgram, sizeof(g_config.instProgram), value);
            } else if (IsValue(key, "Inst Resource")) {
                CopyString(g_config.instResource, sizeof(g_config.instResource), value);
            } else if (IsValue(key, "Inst Movie")) {
                CopyString(g_config.instMovie, sizeof(g_config.instMovie), value);
            } else if (IsValue(key, "Inst BGM")) {
                CopyString(g_config.instBgm, sizeof(g_config.instBgm), value);
            } else if (IsValue(key, "DoubleView")) {
                CopyString(g_config.doubleView, sizeof(g_config.doubleView), value);
            } else if (IsValue(key, "Easy Mode")) {
                CopyString(g_config.easyMode, sizeof(g_config.easyMode), value);
            } else if (IsValue(key, "FullScreen")) {
                CopyString(g_config.fullScreen, sizeof(g_config.fullScreen), value);
            } else if (IsValue(key, "Screen Mode")) {
                CopyString(g_config.screenMode, sizeof(g_config.screenMode), value);
            }
        } else if (lstrcmpiA(section, "Patches") == 0) {
            if (IsValue(key, "BossProjectileFix")) {
                CopyString(g_config.patchBossProjectileFix, sizeof(g_config.patchBossProjectileFix), value);
            } else if (IsValue(key, "Fractional60FpsTimer")) {
                CopyString(g_config.patchFractional60FpsTimer, sizeof(g_config.patchFractional60FpsTimer), value);
            } else if (IsValue(key, "NormalizeScreenMode")) {
                CopyString(g_config.patchNormalizeScreenMode, sizeof(g_config.patchNormalizeScreenMode), value);
            } else if (IsValue(key, "ZeroValuableItemPickupFix")) {
                CopyString(g_config.patchZeroValuableItemPickupFix, sizeof(g_config.patchZeroValuableItemPickupFix), value);
            }
        } else if (lstrcmpiA(section, "KeyConfig") == 0) {
            DWORD decodedSize = 0;

            if (IsValue(key, "GamePad")) {
                if (DecodeHex(value, g_config.gamePad, sizeof(g_config.gamePad), &decodedSize)) {
                    g_config.gamePadSize = decodedSize;
                    g_config.hasGamePad = true;
                }
            } else if (IsValue(key, "Keyboard")) {
                if (DecodeHex(value, g_config.keyboard, sizeof(g_config.keyboard), &decodedSize)) {
                    g_config.keyboardSize = decodedSize;
                    g_config.hasKeyboard = true;
                }
            } else if (IsValue(key, "SideWinder")) {
                if (DecodeHex(value, g_config.sideWinder, sizeof(g_config.sideWinder), &decodedSize)) {
                    g_config.sideWinderSize = decodedSize;
                    g_config.hasSideWinder = true;
                }
            }
        }
    }

    fclose(fp);

    SyncPatchConfigFromPortableStrings();
    NormalizeScreenModeValue(g_config.screenMode, sizeof(g_config.screenMode));
}


static LONG SavePortableConfig()
{
    LoadPortableConfig();

    FILE *fp = fopen(g_configPath, "w");
    if (!fp) {
        LogLine("Portable config write failed: %s", g_configPath);
        return ERROR_ACCESS_DENIED;
    }

    fprintf(fp, "# Mega Man X3 / Rockman X3 portable config\n");
    fprintf(fp, "# MMX3.sav is intentionally stored separately.\n\n");

    fprintf(fp, "[Root]\n");
    fprintf(fp, "CD Drive=%s\n", g_config.cdDrive);
    fprintf(fp, "Inst Program=%s\n", g_config.instProgram);
    fprintf(fp, "Inst Resource=%s\n", g_config.instResource);
    fprintf(fp, "Inst Movie=%s\n", g_config.instMovie);
    fprintf(fp, "Inst BGM=%s\n", g_config.instBgm);
    fprintf(fp, "DoubleView=%s\n", g_config.doubleView);
    fprintf(fp, "Easy Mode=%s\n", g_config.easyMode);
    fprintf(fp, "FullScreen=%s\n", g_config.fullScreen);
    NormalizeScreenModeValue(g_config.screenMode, sizeof(g_config.screenMode));
    CopyString(g_config.patchBossProjectileFix, sizeof(g_config.patchBossProjectileFix), MMX3BoolText(g_patchConfig.bossProjectileFix));
    CopyString(g_config.patchFractional60FpsTimer, sizeof(g_config.patchFractional60FpsTimer), MMX3BoolText(g_patchConfig.fractional60FpsTimer));
    CopyString(g_config.patchNormalizeScreenMode, sizeof(g_config.patchNormalizeScreenMode), MMX3BoolText(g_patchConfig.normalizeScreenMode));
    CopyString(g_config.patchZeroValuableItemPickupFix, sizeof(g_config.patchZeroValuableItemPickupFix), MMX3BoolText(g_patchConfig.zeroValuableItemPickupFix));
    fprintf(fp, "Screen Mode=%s\n", g_config.screenMode);

    fprintf(fp, "\n[Patches]\n");
    fprintf(fp, "BossProjectileFix=%s\n", g_config.patchBossProjectileFix);
    fprintf(fp, "Fractional60FpsTimer=%s\n", g_config.patchFractional60FpsTimer);
    fprintf(fp, "NormalizeScreenMode=%s\n", g_config.patchNormalizeScreenMode);
    fprintf(fp, "ZeroValuableItemPickupFix=%s\n", g_config.patchZeroValuableItemPickupFix);

    fprintf(fp, "\n[KeyConfig]\n");
    WriteHex(fp, "GamePad", g_config.hasGamePad ? g_config.gamePad : kDefaultGamePad,
             g_config.hasGamePad ? g_config.gamePadSize : sizeof(kDefaultGamePad));
    WriteHex(fp, "Keyboard", g_config.hasKeyboard ? g_config.keyboard : kDefaultKeyboard,
             g_config.hasKeyboard ? g_config.keyboardSize : sizeof(kDefaultKeyboard));
    WriteHex(fp, "SideWinder", g_config.hasSideWinder ? g_config.sideWinder : kDefaultSideWinder,
             g_config.hasSideWinder ? g_config.sideWinderSize : sizeof(kDefaultSideWinder));

    fclose(fp);

    LogLine("Portable config saved: %s", g_configPath);
    return ERROR_SUCCESS;
}

static LONG ReturnConfigBinaryOrDefault(
    const char *name,
    const BYTE *configData,
    DWORD configSize,
    bool hasConfigData,
    const BYTE *defaultData,
    DWORD defaultSize,
    LPDWORD lpType,
    LPBYTE lpData,
    LPDWORD lpcbData)
{
    LoadPortableConfig();

    if (hasConfigData && configData && configSize > 0) {
        LogLine("RegQuery KeyConfig %s -> MMX3.conf size=%lu",
                name,
                (unsigned long)configSize);
        return ReturnRegBinary(configData, configSize, lpType, lpData, lpcbData);
    }

    LogLine("RegQuery KeyConfig %s -> default size=%lu",
            name,
            (unsigned long)defaultSize);
    return ReturnRegBinary(defaultData, defaultSize, lpType, lpData, lpcbData);
}


static LONG StoreConfigBinary(
    const char *name,
    const BYTE *src,
    DWORD srcSize,
    BYTE *dst,
    DWORD *dstSize,
    bool *hasDst)
{
    if (!src || srcSize == 0 || srcSize > MMX3_CONFIG_BINARY_MAX || !dst || !dstSize || !hasDst) {
        return ERROR_INVALID_PARAMETER;
    }

    LoadPortableConfig();

    memcpy(dst, src, srcSize);
    *dstSize = srcSize;
    *hasDst = true;

    LogLine("RegSet KeyConfig %s -> MMX3.conf size=%lu",
            name,
            (unsigned long)srcSize);

    return SavePortableConfig();
}

static const char *RootConfigValue(LPCSTR lpValueName)
{
    LoadPortableConfig();

    if (IsValue(lpValueName, "Inst Program")) {
        return g_config.instProgram;
    }
    if (IsValue(lpValueName, "Inst Resource")) {
        return g_config.instResource;
    }
    if (IsValue(lpValueName, "Inst Movie")) {
        return g_config.instMovie;
    }
    if (IsValue(lpValueName, "Inst BGM")) {
        return g_config.instBgm;
    }
    if (IsValue(lpValueName, "DoubleView")) {
        return g_config.doubleView;
    }
    if (IsValue(lpValueName, "Easy Mode")) {
        return g_config.easyMode;
    }
    if (IsValue(lpValueName, "FullScreen")) {
        return g_config.fullScreen;
    }
    if (IsValue(lpValueName, "Screen Mode")) {
        NormalizeScreenModeValue(g_config.screenMode, sizeof(g_config.screenMode)); // RootConfigValue normalize
        return g_config.screenMode;
    }

    return NULL;
}

static bool StoreRootConfigValue(LPCSTR lpValueName, DWORD dwType, const BYTE *lpData, DWORD cbData)
{
    if (!lpValueName || (dwType != REG_SZ && dwType != REG_EXPAND_SZ)) {
        return false;
    }

    LoadPortableConfig();

    char text[128];
    CopyRegSz(text, sizeof(text), lpData, cbData);

    if (IsValue(lpValueName, "CD Drive")) {
        CopyString(g_config.cdDrive, sizeof(g_config.cdDrive), text);
    } else if (IsValue(lpValueName, "Inst Program")) {
        CopyString(g_config.instProgram, sizeof(g_config.instProgram), text);
    } else if (IsValue(lpValueName, "Inst Resource")) {
        CopyString(g_config.instResource, sizeof(g_config.instResource), text);
    } else if (IsValue(lpValueName, "Inst Movie")) {
        CopyString(g_config.instMovie, sizeof(g_config.instMovie), text);
    } else if (IsValue(lpValueName, "Inst BGM")) {
        CopyString(g_config.instBgm, sizeof(g_config.instBgm), text);
    } else if (IsValue(lpValueName, "DoubleView")) {
        CopyString(g_config.doubleView, sizeof(g_config.doubleView), text);
    } else if (IsValue(lpValueName, "Easy Mode")) {
        CopyString(g_config.easyMode, sizeof(g_config.easyMode), text);
    } else if (IsValue(lpValueName, "FullScreen")) {
        CopyString(g_config.fullScreen, sizeof(g_config.fullScreen), text);
    } else if (IsValue(lpValueName, "Screen Mode")) {
        NormalizeScreenModeValue(text, sizeof(text)); // StoreRootConfigValue normalize
        CopyString(g_config.screenMode, sizeof(g_config.screenMode), text);
    } else {
        return false;
    }

    LogLine("RegSet Root stored value=%s data=\"%s\" -> MMX3.conf",
            lpValueName,
            text);

    return SavePortableConfig() == ERROR_SUCCESS;
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

        const char *value = RootConfigValue(lpValueName);
        if (value) {
            LogLine("RegQuery Root %s -> %s", lpValueName, value);
            return ReturnRegSz(value, lpType, lpData, lpcbData);
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
        LoadPortableConfig();

        if (IsValue(lpValueName, "GamePad")) {
            return ReturnConfigBinaryOrDefault(
                "GamePad",
                g_config.gamePad,
                g_config.gamePadSize,
                g_config.hasGamePad,
                kDefaultGamePad,
                sizeof(kDefaultGamePad),
                lpType,
                lpData,
                lpcbData);
        }

        if (IsValue(lpValueName, "Keyboard")) {
            return ReturnConfigBinaryOrDefault(
                "Keyboard",
                g_config.keyboard,
                g_config.keyboardSize,
                g_config.hasKeyboard,
                kDefaultKeyboard,
                sizeof(kDefaultKeyboard),
                lpType,
                lpData,
                lpcbData);
        }

        if (IsValue(lpValueName, "SideWinder")) {
            return ReturnConfigBinaryOrDefault(
                "SideWinder",
                g_config.sideWinder,
                g_config.sideWinderSize,
                g_config.hasSideWinder,
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
            LogLine("RegSet Card Password -> MMX3.sav size=%lu", (unsigned long)cbData);
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
            return StoreConfigBinary(
                "GamePad",
                lpData,
                cbData,
                g_config.gamePad,
                &g_config.gamePadSize,
                &g_config.hasGamePad);
        }

        if (IsValue(lpValueName, "Keyboard") &&
            dwType == REG_BINARY &&
            lpData &&
            cbData > 0) {
            return StoreConfigBinary(
                "Keyboard",
                lpData,
                cbData,
                g_config.keyboard,
                &g_config.keyboardSize,
                &g_config.hasKeyboard);
        }

        if (IsValue(lpValueName, "SideWinder") &&
            dwType == REG_BINARY &&
            lpData &&
            cbData > 0) {
            return StoreConfigBinary(
                "SideWinder",
                lpData,
                cbData,
                g_config.sideWinder,
                &g_config.sideWinderSize,
                &g_config.hasSideWinder);
        }

        LogLine("RegSet KeyConfig ignored value=%s type=%lu size=%lu",
                lpValueName ? lpValueName : "(null)",
                (unsigned long)dwType,
                (unsigned long)cbData);
        return ERROR_SUCCESS;
    }

    if (hKey == FAKE_HKEY_MMX3) {
        if (StoreRootConfigValue(lpValueName, dwType, lpData, cbData)) {
            return ERROR_SUCCESS;
        }

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
