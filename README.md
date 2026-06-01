# Mega Man X3 / Rockman X3 Portable + Bugfix Loader

Source package for a `ddraw.dll` proxy-based portable loader for the PC versions of **Mega Man X3** and **Rockman X3**.

This project makes the game portable by virtualizing the original registry/CD checks, and includes a gameplay bugfix for a long-standing PC-version boss projectile crash.

## Files

| File | Description |
| --- | --- |
| `mmx3_common.h/.cpp` | Shared globals, paths, logging, `PatchIAT`, and `PatchMemory`. |
| `ddraw_proxy.cpp` | DLL entry point, crash logger, and `ddraw.dll` proxy exports. |
| `mmx3_registry.cpp` | Registry virtualization for `MEGAMANX3` and `ROCKMANX3`. |
| `mmx3_cd.cpp` | CD-ROM drive spoofing and fake MCI CDAudio device. |
| `mmx3_bugfix.cpp` | Gameplay bugfixes and optional debug instrumentation. |
| `ddraw_proxy.def` | Export definition file. |
| `build.bat` | Default build; calls `build_debug.bat`. |
| `build_debug.bat` | Debug build, logging enabled. Can be double-clicked. |
| `build_release.bat` | Release-style build, logging disabled. Can be double-clicked. |
| `_build_common.bat` | Shared build helper that locates MSVC automatically. |

## Build

You can usually build by double-clicking one of these batch files:

```bat
build.bat
build_debug.bat
build_release.bat
```

The build script will:

1. Check whether `cl.exe` is already available.
2. If not, locate Visual Studio / Build Tools with `vswhere.exe`.
3. Initialize an x86 MSVC environment with `VsDevCmd.bat -arch=x86`.
4. Build `ddraw.dll`.

Required toolchain:

```text
Visual Studio or Build Tools for Visual Studio
Desktop development with C++
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
MMX3.conf
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

## Portable behavior

The loader redirects the original registry-based save/config behavior to local files next to the game executable:

```text
MMX3.sav
MMX3.conf
```

`MMX3.sav` stores the password/save-card data only. `MMX3.conf` stores the remaining portable configuration, including root registry options such as `Easy Mode`, `Screen Mode`, `FullScreen`, `DoubleView`, and the three `KeyConfig` binary blobs.

This allows the game to run without requiring the original installer registry state.

## CD audio / CD-ROM behavior

The fake CD audio path intentionally keeps the original MCI wrapper flow alive instead of directly bypassing the CD audio initialization logic.

The loader spoofs the game directory drive as a CD-ROM drive and provides a fake MCI CDAudio device for the original CD audio checks.

The only CD-audio-related internal code patch currently used is:

```text
IsMciDeviceOpen -> true
RVA: 0x00003C00
```

This allows the fake MCI CDAudio device to handle status/play/stop requests even when the original wrapper has not opened a real CD audio device.

## Gameplay bugfixes

### Boss projectile crash fix

The PC version has a boss projectile bug where a special projectile can crash the game after the player jumps behind the boss.

The crash was traced to a missing argument before a movement helper call:

```text
00476E60  CALL 0042BCC7
```

`FUN_0042BCC7` expects one object pointer argument, but the original PC code calls it without pushing an argument. This causes the movement routine to receive stack garbage such as `0x00000000` or `0x00000040`, eventually crashing at:

```text
0042BCEC  MOV CX, word ptr [EAX + 0x6]
```

The minimal fix replaces the original 5-byte call at `00476E60` with a trampoline:

```asm
push dword ptr [ebp+8]
call 0042BCC7
add  esp, 4
jmp  00476E65
```

The important detail is that the movement helper must receive the parent projectile object (`[ebp+8]`), not the child data block at `parent + 0x40`.

This restores the projectile movement/disappear behavior and prevents the boss AI from getting stuck.

### Debug projectile logger

`mmx3_bugfix.cpp` also contains an optional projectile state logger for `FUN_00476E46`.

It is enabled only when logging is enabled:

```cpp
#define MMX3_ENABLE_PROJECTILE_STATE_LOG MMX3_ENABLE_LOG
```

So:

```text
build_debug.bat   -> logger enabled
build_release.bat -> logger disabled
```

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

## Notes

This project is intended as a compatibility/bugfix loader for legitimately owned PC versions of Mega Man X3 / Rockman X3.
