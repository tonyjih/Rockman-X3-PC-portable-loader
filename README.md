# Mega Man X3 / Rockman X3 Portable Loader

Source package for a `ddraw.dll` proxy-based portable loader for the PC versions of **Mega Man X3** and **Rockman X3**.

## Files

| File | Description |
| --- | --- |
| `mmx3_common.h/.cpp` | Shared globals, paths, logging, `PatchIAT`, and `PatchMemory`. |
| `ddraw_proxy.cpp` | DLL entry point and `ddraw.dll` proxy exports. |
| `mmx3_registry.cpp` | Registry virtualization for `MEGAMANX3` and `ROCKMANX3`. |
| `mmx3_cd.cpp` | CD-ROM drive spoofing and fake MCI CDAudio device. |
| `ddraw_proxy.def` | Export definition file. |
| `build.bat` | Default MSVC x86 build command, logging enabled. |
| `build_debug.bat` | Explicit debug build, logging enabled. |
| `build_release.bat` | Release-style build, logging disabled. |

## Build

Open an **x86 Native Tools Command Prompt for Visual Studio**.

Run one of:

```bat
build.bat
build_debug.bat
build_release.bat
```

Expected output:

```text
ddraw.dll
```

Place `ddraw.dll` next to `MMX3.exe` or `RMX3.exe`.

## Runtime outputs

The loader may create the following files next to the game executable:

```text
MMX3.sav
KeyConfig_GamePad.bin
KeyConfig_Keyboard.bin
KeyConfig_SideWinder.bin
mmx3_portable.log
```

`mmx3_portable.log` is created only by debug/log-enabled builds.

## Supported registry keys

The loader virtualizes these registry paths:

```text
HKCU\Software\CAPCOM\MEGAMANX3
HKCU\Software\CAPCOM\ROCKMANX3
```

Supported subkeys include:

```text
Card
KeyConfig
```

## Notes

The fake CD audio path intentionally keeps the original MCI wrapper flow alive instead of directly bypassing the CD audio initialization logic.

The only internal code patch currently used is:

```text
IsMciDeviceOpen -> true
RVA: 0x00003C00
```

This allows the fake MCI CDAudio device to handle status/play/stop requests even when the original wrapper has not opened a real CD audio device.

## Logging

Logging is controlled by `MMX3_ENABLE_LOG` in `mmx3_common.h`.

Default:

```cpp
#define MMX3_ENABLE_LOG 1
```

`build_debug.bat` passes:

```bat
/DMMX3_ENABLE_LOG=1
```

`build_release.bat` passes:

```bat
/DMMX3_ENABLE_LOG=0
```

When logging is disabled, `mmx3_portable.log` is not created.
