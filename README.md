# Mega Man X3 / Rockman X3 Portable + Bugfix Loader

Source package for a `ddraw.dll` proxy-based portable loader for the 1997 PC versions of **Mega Man X3** and **Rockman X3**.

This project makes the game portable by virtualizing the original registry/CD checks, stores save/config data next to the game executable, includes a gameplay bugfix for a long-standing PC-version boss projectile crash, and contains an optional main-timer patch for smoother 60 FPS pacing.

## Files

| File | Description |
| --- | --- |
| `mmx3_common.h/.cpp` | Shared globals, paths, logging, `PatchIAT`, and `PatchMemory`. |
| `ddraw_proxy.cpp` | DLL entry point, crash logger, loader initialization, and `ddraw.dll` proxy exports. |
| `mmx3_registry.cpp` | Registry virtualization for `MEGAMANX3` and `ROCKMANX3`, backed by portable local files. |
| `mmx3_cd.cpp` | CD-ROM drive spoofing and fake MCI CDAudio device. |
| `mmx3_bugfix.cpp` | Gameplay bugfixes and optional debug instrumentation. |
| `mmx3_timing.cpp` | Timing API logging hooks and optional main-timer experiments, including the fractional 60 FPS timer hook. |

## Build

Build as a 32-bit Windows DLL named:

```text
ddraw.dll
```

Required toolchain:

```text
Visual Studio or Build Tools for Visual Studio
Desktop development with C++
x86 target
```

Place the resulting `ddraw.dll` next to `MMX3.exe` or `RMX3.exe`.

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

Root registry values currently persisted into `MMX3.conf` include:

```text
CD Drive
Easy Mode
Screen Mode
FullScreen
DoubleView
```

## Portable behavior

The loader redirects the original registry-based save/config behavior to local files next to the game executable:

```text
MMX3.sav
MMX3.conf
```

`MMX3.sav` stores the password/save-card data only.

`MMX3.conf` stores the remaining portable configuration, including root registry options such as `CD Drive`, `Easy Mode`, `Screen Mode`, `FullScreen`, `DoubleView`, and the three `KeyConfig` binary blobs:

```text
GamePad
Keyboard
SideWinder
```

Older standalone key-config files are intentionally not used. The current portable layout keeps all non-save configuration in `MMX3.conf`.

This allows the game to run without requiring the original installer registry state.

## Runtime patch switches

`MMX3.conf` can also control runtime patches through a `[Patches]` section:

```ini
[Patches]
BossProjectileFix=True
Fractional60FpsTimer=True
NormalizeScreenMode=True
```

The defaults keep all current compatibility fixes enabled. Set a value to `False` to disable that patch for testing or to reproduce original behavior.

| Key | Effect |
| --- | --- |
| `BossProjectileFix` | Enables the PC boss projectile crash fix. |
| `Fractional60FpsTimer` | Enables the fractional 60 FPS main-timer thread hook when the build timer mode is `MMX3_TIMER_MODE_FRACTIONAL_60`. |
| `NormalizeScreenMode` | Normalizes problematic `Screen Mode=640,480,8` config data to `640,480,32` to avoid the broken modern fullscreen path. |

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

## Main timer / 60 FPS behavior

`mmx3_timing.cpp` contains timing API hooks for investigation and an optional main-timer experiment.

The timer mode is selected in `mmx3_timing.cpp`:

```cpp
#define MMX3_TIMER_MODE_ORIGINAL       0
#define MMX3_TIMER_MODE_PATCH_16MS     1
#define MMX3_TIMER_MODE_FRACTIONAL_60  2

#define MMX3_TIMER_MODE MMX3_TIMER_MODE_FRACTIONAL_60
```

### Mode 0: original

Leaves the original game timing code unchanged.

### Mode 1: 16 ms immediate patch

Patches the original main timer setup from `17 ms` to `16 ms`:

```text
00402354: push 11h -> push 10h
```

This is simple, but pure 16 ms pacing is not exactly 60 FPS.

### Mode 2: fractional 60 FPS thread hook

Replaces the original main timer thread entry at:

```text
004DEB10
```

with a hook that runs the timer callback using a fractional 60 FPS cadence:

```text
16 / 17 / 17 ms repeating pattern
```

This approximates 1000 ms / 60 frames while keeping the original callback flow alive. Debug builds periodically log stats such as active FPS and drift.

## Timing logging

When logging is enabled, `mmx3_timing.cpp` can log calls to:

```text
Sleep
GetTickCount
timeGetTime
QueryPerformanceCounter
QueryPerformanceFrequency
```

Known main-timer `timeGetTime` caller labels include:

```text
004DEB37  MainTimer.base
004DEB4A  MainTimer.wait
004DE1BA  Timer/controller-side caller
```

These labels are for debug investigation only and are not required for release-style builds.

## Logging

Logging is controlled by `MMX3_ENABLE_LOG` in `mmx3_common.h`.

Default:

```cpp
#define MMX3_ENABLE_LOG 1
```

When logging is disabled, `mmx3_portable.log` is not created, and optional debug instrumentation is compiled out or kept quiet depending on the surrounding feature macro.

## Notes

This project is intended as a compatibility/bugfix loader for legitimately owned PC versions of Mega Man X3 / Rockman X3.
