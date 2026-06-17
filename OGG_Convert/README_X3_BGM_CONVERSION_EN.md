# Mega Man X3 / Rockman X3 PS Audio CD BGM Conversion Guide

This guide explains how to convert the **PlayStation version of Mega Man X3 / Rockman X3 CD-DA music tracks** into OGG files that can be used by the PC version.

The toolchain will:

```text
Ripped WAV tracks
→ Trim looped tracks at their LOOP_END point
→ Encode them as OGG Vorbis
→ Write LOOP_START / LOOP_END / LOOP_LENGTH metadata
→ Rename the output files to the PC version naming scheme: X3_XX.ogg
```

> Please use your own legally owned PlayStation game disc.  
> Do not distribute the converted music files.

---

## Important compatibility note

The included loop points were measured and tested with the **Japanese PlayStation release**:

```text
Rockman X3
SLPS-00283
```

Other releases, such as the North American or European PlayStation versions, may have different CD-DA track timing, track layout, pregap behavior, or mastering differences.  
If you use a release other than **SLPS-00283**, the conversion script may still run, but the loop points may not line up correctly.

For best results, use **SLPS-00283**.  
If you use another version, please verify every converted track manually with foobar2000 + vgmstream.

---

## Recommended environment

This workflow was tested with:

```text
foobar2000 for ripping / WAV conversion
Python 3
FFmpeg
make_loop_ogg.py
x3_loop_points.txt
```

You can use another CD ripping tool if you prefer, but this guide recommends **foobar2000**, because that is the tested environment.

---

## Required files and tools

You need:

1. A PlayStation copy of **Mega Man X3 / Rockman X3**.
2. **foobar2000**, recommended for ripping the CD-DA tracks to WAV.
3. **Python 3**.
4. **FFmpeg**, with `ffmpeg` available from the command line.
5. `make_loop_ogg.py`
6. `x3_loop_points.txt`

Place `make_loop_ogg.py`, `x3_loop_points.txt`, and the ripped WAV files in the same working folder.

---

## Step 1: Rip the PS CD-DA tracks to WAV

Use foobar2000 to rip or convert the audio CD tracks to WAV.

Recommended output format:

```text
WAV
44100 Hz
16-bit
Stereo
```

Do not convert the tracks to MP3 first.  
Do not apply ReplayGain, normalization, fade, DSP effects, resampling, or any other audio processing.

The script expects the ripped files to be named like this:

```text
CD Track 01.wav
CD Track 02.wav
CD Track 03.wav
...
```

If your files are named differently, either rename them or change the `--source-pattern` argument when running the script.

Example foobar2000 filename pattern:

```text
CD Track $num(%tracknumber%,2)
```

This should produce names such as:

```text
CD Track 01.wav
CD Track 02.wav
CD Track 03.wav
```

If foobar2000 does not detect the audio tracks on your drive, use another CD-DA ripping tool and make sure the final WAV files follow the expected naming scheme.

---

## Step 2: Run the conversion script

Open a command prompt or PowerShell window in the folder containing the WAV files.

Run:

```powershell
python make_loop_ogg.py "./x3_loop_points.txt" --src "./" --out "./BGM_EXT" --source-pattern "CD Track {track}.wav" --include-unlisted --x3-pc-names --overwrite
```

After the conversion, the output folder should contain:

```text
BGM_EXT/
  X3_00.ogg
  X3_01.ogg
  X3_02.ogg
  ...
```

These files are the converted OGG BGM files intended for the PC version.

---

## What the command does

### `./x3_loop_points.txt`

This is the loop point definition file.

It contains the tracks that need loop trimming and their `LOOP_START` / `LOOP_END` positions.

The provided loop points are intended for **Rockman X3 SLPS-00283**, the Japanese PlayStation release.

Example:

```text
02
0:22.662
1:39.195

03
0:27.178
1:43.922
```

This means:

```text
CD Track 02.wav
LOOP_START = 0:22.662
LOOP_END   = 1:39.195
```

For looped tracks, the script keeps the audio from the beginning of the file to `LOOP_END`, then removes the remaining repeated section and fade-out.

The final structure becomes:

```text
Intro + one clean loop section
        ↑                  ↑
        LOOP_START         LOOP_END / file end
```

---

### `--src "./"`

Sets the source folder for the WAV files.

`"./"` means the current folder.

---

### `--out "./BGM_EXT"`

Sets the output folder.

The converted OGG files will be written to:

```text
BGM_EXT
```

---

### `--source-pattern "CD Track {track}.wav"`

Sets the expected input filename pattern.

`{track}` is replaced by a two-digit track number.

Examples:

```text
CD Track 02.wav
CD Track 03.wav
CD Track 10.wav
```

If your files are named differently, adjust this argument.

For example, if your files are named:

```text
Track 02.wav
```

Use:

```powershell
--source-pattern "Track {track}.wav"
```

If your files are named:

```text
02.wav
```

Use:

```powershell
--source-pattern "{track}.wav"
```

---

### `--include-unlisted`

Also converts WAV files that are not listed in `x3_loop_points.txt`.

Unlisted tracks are treated as non-looping tracks.

They will be:

```text
Converted to OGG
Not trimmed
Not given loop tags
```

This is useful for tracks that do not need seamless looping.

---

### `--x3-pc-names`

Uses the PC version output naming scheme.

The mapping is:

```text
CD Track 01.wav → X3_00.ogg
CD Track 02.wav → X3_01.ogg
CD Track 03.wav → X3_02.ogg
...
```

In other words:

```text
X3_{track number - 1}.ogg
```

with a two-digit number.

---

### `--overwrite`

Allows the script to overwrite existing output files.

Remove this option if you want to avoid overwriting existing files.

---

## Output loop metadata

For tracks listed in `x3_loop_points.txt`, the script writes both common loop tag styles:

```text
LOOP_START
LOOP_END
LOOP_LENGTH

LOOPSTART
LOOPEND
LOOPLENGTH
```

Where:

```text
LOOP_LENGTH = LOOP_END - LOOP_START
```

The underscore style is useful for vgmstream-compatible playback:

```text
LOOP_START
LOOP_END
```

The non-underscore style is included for compatibility with other tools and engines:

```text
LOOPSTART
LOOPEND
LOOPLENGTH
```

---

## Testing the converted files

Recommended test player:

```text
foobar2000 + vgmstream component
```

Normal music players usually ignore OGG loop tags.

A normal player may play like this:

```text
Beginning → file end → beginning
```

A loop-aware player should play like this:

```text
Beginning → LOOP_END → LOOP_START → LOOP_END → LOOP_START ...
```

If you are testing with foobar2000, make sure the vgmstream component is installed and that it is actually handling the OGG files.

---

## Dry run

To check what the script will do without writing files, add:

```powershell
--dry-run
```

Example:

```powershell
python make_loop_ogg.py "./x3_loop_points.txt" --src "./" --out "./BGM_EXT" --source-pattern "CD Track {track}.wav" --include-unlisted --x3-pc-names --dry-run
```

---

## Changing OGG quality

The default OGG Vorbis quality is:

```text
--quality 6
```

This is usually good enough for CD-DA game BGM.

If you want higher quality, use:

```powershell
--quality 7
```

Example:

```powershell
python make_loop_ogg.py "./x3_loop_points.txt" --src "./" --out "./BGM_EXT" --source-pattern "CD Track {track}.wav" --include-unlisted --x3-pc-names --overwrite --quality 7
```

---

## Troubleshooting

### `ffmpeg` is not found

Check that FFmpeg is installed and available in your system PATH.

Test with:

```powershell
ffmpeg -version
```

If the command prints version information, FFmpeg is set up correctly.

---

### The script cannot find the WAV files

Check whether the filenames match the pattern:

```text
CD Track 02.wav
```

If not, either rename the WAV files or change `--source-pattern`.

Example:

```powershell
--source-pattern "Track {track}.wav"
```

or:

```powershell
--source-pattern "{track}.wav"
```

---

### The loop does not work in foobar2000

Make sure you are using the vgmstream component.

The OGG loop tags are metadata conventions, not standard OGG playback behavior.  
A player must explicitly support these tags to loop correctly.

---

### Some tracks do not have loop tags

This is expected for tracks that are not listed in `x3_loop_points.txt`.

Those tracks are converted as full non-looping OGG files.

---

## Suggested folder layout

Before conversion:

```text
X3_PS_WAV/
  make_loop_ogg.py
  x3_loop_points.txt
  CD Track 01.wav
  CD Track 02.wav
  CD Track 03.wav
  ...
```

After conversion:

```text
X3_PS_WAV/
  BGM_EXT/
    X3_00.ogg
    X3_01.ogg
    X3_02.ogg
    ...
```

Copy the generated OGG files from `BGM_EXT` into the PC version's BGM folder.

---

## One-command summary

For the standard tested workflow, run:

```powershell
python make_loop_ogg.py "./x3_loop_points.txt" --src "./" --out "./BGM_EXT" --source-pattern "CD Track {track}.wav" --include-unlisted --x3-pc-names --overwrite
```
