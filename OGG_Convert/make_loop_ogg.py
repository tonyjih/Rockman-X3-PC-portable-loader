#!/usr/bin/env python3
"""
make_loop_ogg_v4.py

Batch converts CD-DA/WAV/FLAC/etc. BGM tracks to Ogg Vorbis.

For tracks listed in the points file:
  - trim the source at LOOP_END so the output is Intro + one loop section
  - write common loop tags for vgmstream and other tools

For tracks not listed in the points file, when --include-unlisted is used:
  - encode the full source file
  - do not write loop tags

Loop point file formats supported:

1) Three-line block format:

02
0:22.662
1:39.195

03
0:12.690
0:57.757

2) One-line format:

02 0:22.662 1:39.195
03,0:12.690,0:57.757

Times may be:
  ss.xxx
  mm:ss.xxx
  hh:mm:ss.xxx

Sample positions are also supported with an explicit prefix:
  sample:559629
  samples:2547084
  smp:1987455

Example:
  py make_loop_ogg_v4.py loop_points.txt --src "D:\\X3\\wav" --out "D:\\X3\\ogg" --quality 6

Include unlisted source tracks as no-loop OGGs:
  py make_loop_ogg_v4.py loop_points.txt --src . --out .\\ogg --source-pattern "CD Track {track}.wav" --include-unlisted

If auto source matching is ambiguous, use --source-pattern:
  py make_loop_ogg_v4.py loop_points.txt --src "D:\\X3\\wav" --out "D:\\X3\\ogg" --source-pattern "Track {track}.wav"

Placeholders for patterns/output names:
  {track}         original track id, e.g. 02
  {track_int}     integer track id, e.g. 2
  {track2}        zero-padded 2-digit id, e.g. 02
  {track3}        zero-padded 3-digit id, e.g. 002
  {track_index0}  zero-based integer index, e.g. track 02 -> 1
  {track0_2}      zero-based, zero-padded 2-digit index, e.g. track 02 -> 01
  {track0_3}      zero-based, zero-padded 3-digit index, e.g. track 02 -> 001
  {stem}          source file stem, e.g. Track 02

For the PC version of Rockman X3/Mega Man X3, use:
  --x3-pc-names
This outputs CD Track 01.wav as X3_00.ogg, CD Track 02.wav as X3_01.ogg, etc.
"""

from __future__ import annotations

import argparse
import csv
import json
import re
import subprocess
import sys
from dataclasses import dataclass
from decimal import Decimal, ROUND_HALF_UP, InvalidOperation
from pathlib import Path
from typing import Iterable, Optional

AUDIO_EXTS = {
    ".wav",
    ".wave",
    ".flac",
    ".aiff",
    ".aif",
    ".aifc",
    ".caf",
    ".ogg",
    ".oga",
}

LOOP_TAGS = [
    "LOOP_START",
    "LOOP_END",
    "LOOP_LENGTH",
    "LOOPSTART",
    "LOOPEND",
    "LOOPLENGTH",
]


@dataclass(frozen=True)
class LoopSpec:
    track: str
    start_text: str
    end_text: str
    line_no: int


@dataclass(frozen=True)
class TrackJob:
    track: str
    source: Path
    output: Path
    sample_rate: int
    is_looped: bool
    start_text: str = ""
    end_text: str = ""
    loop_start: int = 0
    loop_end: int = 0
    loop_length: int = 0


class UserError(Exception):
    pass


def run_cmd(cmd: list[str], *, capture: bool = False) -> subprocess.CompletedProcess[str]:
    try:
        return subprocess.run(
            cmd,
            check=True,
            text=True,
            stdout=subprocess.PIPE if capture else None,
            stderr=subprocess.PIPE if capture else None,
        )
    except FileNotFoundError as exc:
        raise UserError(
            f"Command not found: {cmd[0]}. Install FFmpeg and make sure ffmpeg/ffprobe are in PATH."
        ) from exc
    except subprocess.CalledProcessError as exc:
        if capture:
            msg = (exc.stderr or exc.stdout or str(exc)).strip()
        else:
            msg = str(exc)
        raise UserError(f"Command failed: {' '.join(cmd)}\n{msg}") from exc


def clean_line(line: str) -> str:
    # Remove inline comments. This points file is intentionally simple, so '#' is reserved for comments.
    return line.split("#", 1)[0].strip()


def parse_points_file(path: Path) -> list[LoopSpec]:
    specs: list[LoopSpec] = []
    pending: list[tuple[str, int]] = []

    with path.open("r", encoding="utf-8-sig", newline="") as f:
        for line_no, raw_line in enumerate(f, 1):
            line = clean_line(raw_line)
            if not line:
                continue

            # Let CSV handle commas and quoted fields, then fall back to whitespace splitting.
            if "," in line:
                row = next(csv.reader([line]))
                tokens = [t.strip() for t in row if t.strip()]
            else:
                tokens = re.split(r"\s+", line)

            if not tokens:
                continue

            # Skip obvious headers.
            if tokens[0].lower() in {"track", "bgm", "id", "no", "number"}:
                continue

            if len(tokens) >= 3:
                specs.append(LoopSpec(tokens[0], tokens[1], tokens[2], line_no))
                continue

            if len(tokens) == 1:
                pending.append((tokens[0], line_no))
                if len(pending) == 3:
                    specs.append(LoopSpec(pending[0][0], pending[1][0], pending[2][0], pending[0][1]))
                    pending.clear()
                continue

            raise UserError(f"Cannot parse line {line_no}: {raw_line.rstrip()}")

    if pending:
        lines = ", ".join(str(no) for _, no in pending)
        raise UserError(f"Incomplete 3-line loop block near line(s): {lines}")

    if not specs:
        raise UserError(f"No loop points found in: {path}")

    return specs


def track_int_or_none(track: str) -> Optional[int]:
    # Common IDs are "02" or "BGM02". Use the last digit run for convenience.
    m = re.search(r"(\d+)(?!.*\d)", track)
    if not m:
        return None
    return int(m.group(1))


def track_id_from_source(path: Path) -> str:
    # "CD Track 02.wav" -> "02". If no digits exist, use the stem.
    m = re.search(r"(\d+)(?!.*\d)", path.stem)
    if not m:
        return path.stem
    return m.group(1)


def track_sort_key(track: str) -> tuple[int, object]:
    n = track_int_or_none(track)
    if n is None:
        return (1, track.lower())
    return (0, n)


def path_sort_key(path: Path) -> tuple[int, object, str]:
    return (*track_sort_key(track_id_from_source(path)), path.name.lower())


def pattern_values(track: str, source_stem: str = "") -> dict[str, object]:
    n = track_int_or_none(track)
    values: dict[str, object] = {
        "track": track,
        "stem": source_stem,
    }
    if n is not None:
        index0 = n - 1
        values.update(
            {
                "track_int": n,
                "track2": f"{n:02d}",
                "track3": f"{n:03d}",
                "track_index0": index0,
                "track0_2": f"{index0:02d}",
                "track0_3": f"{index0:03d}",
            }
        )
    else:
        values.update(
            {
                "track_int": track,
                "track2": track,
                "track3": track,
                "track_index0": track,
                "track0_2": track,
                "track0_3": track,
            }
        )
    return values


def format_pattern(pattern: str, track: str, source_stem: str = "") -> str:
    try:
        return pattern.format(**pattern_values(track, source_stem))
    except KeyError as exc:
        raise UserError(f"Unknown placeholder in pattern '{pattern}': {exc}") from exc


def is_under(path: Path, parent: Path) -> bool:
    try:
        path.resolve().relative_to(parent.resolve())
        return True
    except ValueError:
        return False


def iter_audio_files(src_dir: Path, recursive: bool, *, skip_dir: Optional[Path] = None) -> Iterable[Path]:
    globber = src_dir.rglob("*") if recursive else src_dir.glob("*")
    for p in globber:
        if skip_dir is not None and is_under(p, skip_dir):
            continue
        if p.is_file() and p.suffix.lower() in AUDIO_EXTS:
            yield p


def find_source(src_dir: Path, track: str, source_pattern: Optional[str], recursive: bool) -> Path:
    if source_pattern:
        rel = format_pattern(source_pattern, track)
        p = Path(rel)
        if not p.is_absolute():
            p = src_dir / p
        if not p.exists():
            raise UserError(f"Source file not found for track {track}: {p}")
        if not p.is_file():
            raise UserError(f"Source path is not a file for track {track}: {p}")
        return p

    n = track_int_or_none(track)
    names: list[str] = [track]
    if n is not None:
        names.extend(
            [
                f"{n}",
                f"{n:02d}",
                f"{n:03d}",
                f"track{n}",
                f"track{n:02d}",
                f"track_{n}",
                f"track_{n:02d}",
                f"Track {n}",
                f"Track {n:02d}",
                f"Track{n}",
                f"Track{n:02d}",
                f"CD Track {n}",
                f"CD Track {n:02d}",
                f"BGM{n}",
                f"BGM{n:02d}",
                f"bgm{n}",
                f"bgm{n:02d}",
            ]
        )
    # Preserve order, remove duplicates case-insensitively.
    seen_names: set[str] = set()
    names = [x for x in names if not (x.lower() in seen_names or seen_names.add(x.lower()))]

    files = list(iter_audio_files(src_dir, recursive))
    by_stem = {p.stem.lower(): p for p in files}

    for name in names:
        p = by_stem.get(name.lower())
        if p:
            return p

    # Prefix match such as "02 - Opening Stage.wav".
    prefix_candidates: list[Path] = []
    prefixes: list[str] = []
    for name in names:
        prefixes.extend([f"{name} -", f"{name}_", f"{name} "])
    prefixes = list(dict.fromkeys(x.lower() for x in prefixes))

    for p in files:
        stem_l = p.stem.lower()
        if any(stem_l.startswith(prefix) for prefix in prefixes):
            prefix_candidates.append(p)

    if len(prefix_candidates) == 1:
        return prefix_candidates[0]
    if len(prefix_candidates) > 1:
        choices = "\n  ".join(str(p) for p in prefix_candidates[:20])
        raise UserError(
            f"Multiple source candidates for track {track}. Use --source-pattern to disambiguate.\n  {choices}"
        )

    raise UserError(
        f"No source file found for track {track} in {src_dir}. "
        "Use --source-pattern, for example: --source-pattern \"Track {track}.wav\""
    )


def ffprobe_sample_rate(path: Path) -> int:
    cmd = [
        "ffprobe",
        "-v",
        "error",
        "-select_streams",
        "a:0",
        "-show_entries",
        "stream=sample_rate",
        "-of",
        "default=noprint_wrappers=1:nokey=1",
        str(path),
    ]
    proc = run_cmd(cmd, capture=True)
    text = (proc.stdout or "").strip().splitlines()
    if not text:
        raise UserError(f"Could not read sample rate from: {path}")
    try:
        return int(text[0].strip())
    except ValueError as exc:
        raise UserError(f"Invalid sample rate from ffprobe for {path}: {text[0]}") from exc


def parse_time_to_samples(text: str, sample_rate: int) -> int:
    raw = text.strip()
    lower = raw.lower()
    for prefix in ("samples:", "sample:", "smp:"):
        if lower.startswith(prefix):
            value = raw[len(prefix) :].strip()
            if not re.fullmatch(r"\d+", value):
                raise UserError(f"Invalid sample value: {raw}")
            return int(value)

    parts = raw.split(":")
    if not 1 <= len(parts) <= 3:
        raise UserError(f"Invalid time format: {raw}")

    try:
        dec_parts = [Decimal(p.strip()) for p in parts]
    except InvalidOperation as exc:
        raise UserError(f"Invalid time number: {raw}") from exc

    if len(dec_parts) == 1:
        seconds = dec_parts[0]
    elif len(dec_parts) == 2:
        seconds = dec_parts[0] * Decimal(60) + dec_parts[1]
    else:
        seconds = dec_parts[0] * Decimal(3600) + dec_parts[1] * Decimal(60) + dec_parts[2]

    if seconds < 0:
        raise UserError(f"Negative time is not valid: {raw}")

    samples = (seconds * Decimal(sample_rate)).to_integral_value(rounding=ROUND_HALF_UP)
    return int(samples)


def make_output_path(out_dir: Path, output_name: str, track: str, source: Path) -> Path:
    filename = format_pattern(output_name, track, source.stem)
    if not filename.lower().endswith(".ogg"):
        filename += ".ogg"
    p = Path(filename)
    if not p.is_absolute():
        p = out_dir / p
    return p


def build_loop_job(spec: LoopSpec, src_dir: Path, out_dir: Path, args: argparse.Namespace) -> TrackJob:
    source = find_source(src_dir, spec.track, args.source_pattern, args.recursive)
    sample_rate = args.sample_rate if args.sample_rate else ffprobe_sample_rate(source)
    start = parse_time_to_samples(spec.start_text, sample_rate)
    end = parse_time_to_samples(spec.end_text, sample_rate)
    if end <= start:
        raise UserError(
            f"Track {spec.track}: LOOP_END must be greater than LOOP_START. "
            f"start={start}, end={end}"
        )
    length = end - start
    output = make_output_path(out_dir, args.output_name, spec.track, source)
    if output.resolve() == source.resolve():
        raise UserError(f"Output would overwrite source for track {spec.track}: {output}")
    return TrackJob(
        track=spec.track,
        source=source,
        output=output,
        sample_rate=sample_rate,
        is_looped=True,
        start_text=spec.start_text,
        end_text=spec.end_text,
        loop_start=start,
        loop_end=end,
        loop_length=length,
    )


def build_plain_job(source: Path, out_dir: Path, args: argparse.Namespace) -> TrackJob:
    track = track_id_from_source(source)
    sample_rate = args.sample_rate if args.sample_rate else ffprobe_sample_rate(source)
    output = make_output_path(out_dir, args.output_name, track, source)
    if output.resolve() == source.resolve():
        raise UserError(f"Output would overwrite source for no-loop track {track}: {output}")
    return TrackJob(
        track=track,
        source=source,
        output=output,
        sample_rate=sample_rate,
        is_looped=False,
    )


def all_loop_metadata_args(job: TrackJob) -> list[str]:
    return [
        "-metadata",
        f"LOOP_START={job.loop_start}",
        "-metadata",
        f"LOOP_END={job.loop_end}",
        "-metadata",
        f"LOOP_LENGTH={job.loop_length}",
        "-metadata",
        f"LOOPSTART={job.loop_start}",
        "-metadata",
        f"LOOPEND={job.loop_end}",
        "-metadata",
        f"LOOPLENGTH={job.loop_length}",
    ]


def clear_loop_metadata_args() -> list[str]:
    args: list[str] = []
    for tag in LOOP_TAGS:
        args.extend(["-metadata", f"{tag}="])
        args.extend(["-metadata:s:a:0", f"{tag}="])
    return args


def encode_job(job: TrackJob, args: argparse.Namespace) -> None:
    job.output.parent.mkdir(parents=True, exist_ok=True)

    overwrite_flag = "-y" if args.overwrite else "-n"

    cmd = [
        "ffmpeg",
        "-hide_banner",
        overwrite_flag,
        "-i",
        str(job.source),
        "-map",
        "0:a:0",
        "-vn",
        "-map_metadata",
        "0",
    ]

    if job.is_looped:
        afilter = f"atrim=end_sample={job.loop_end},asetpts=N/SR/TB"
        cmd.extend(["-af", afilter])

    cmd.extend(
        [
            "-ar",
            str(job.sample_rate),
            "-c:a",
            "libvorbis",
            "-q:a",
            str(args.quality),
        ]
    )

    if job.is_looped:
        cmd.extend(all_loop_metadata_args(job))
    else:
        cmd.extend(clear_loop_metadata_args())

    cmd.append(str(job.output))

    if args.dry_run:
        print("DRY-RUN ffmpeg command:")
        print("  " + subprocess.list2cmdline(cmd))
        return

    run_cmd(cmd, capture=False)


def read_all_tags(path: Path) -> dict[str, str]:
    cmd = [
        "ffprobe",
        "-v",
        "error",
        "-show_entries",
        "format_tags:stream_tags",
        "-of",
        "json",
        str(path),
    ]
    proc = run_cmd(cmd, capture=True)
    try:
        data = json.loads(proc.stdout or "{}")
    except json.JSONDecodeError as exc:
        raise UserError(f"ffprobe returned invalid JSON for {path}") from exc

    raw_tags: dict[str, object] = {}
    raw_tags.update(data.get("format", {}).get("tags", {}) or {})
    for stream in data.get("streams", []) or []:
        raw_tags.update(stream.get("tags", {}) or {})

    # Vorbis comments are case-insensitive in practice, and ffprobe may normalize
    # some names, so compare by uppercase key names.
    return {str(k).upper(): str(v) for k, v in raw_tags.items()}


def verify_loop_job(job: TrackJob) -> None:
    tags = read_all_tags(job.output)

    missing = [tag for tag in LOOP_TAGS if tag.upper() not in tags]
    if missing:
        raise UserError(f"Output missing tag(s) for {job.output}: {', '.join(missing)}")

    expected = {
        "LOOP_START": str(job.loop_start),
        "LOOP_END": str(job.loop_end),
        "LOOP_LENGTH": str(job.loop_length),
        "LOOPSTART": str(job.loop_start),
        "LOOPEND": str(job.loop_end),
        "LOOPLENGTH": str(job.loop_length),
    }
    bad = [k for k, v in expected.items() if tags.get(k.upper()) != v]
    if bad:
        details = ", ".join(f"{k}={tags.get(k.upper())!r}, expected {expected[k]!r}" for k in bad)
        raise UserError(f"Output has incorrect loop tag(s) for {job.output}: {details}")


def verify_plain_job(job: TrackJob) -> None:
    tags = read_all_tags(job.output)
    present = [tag for tag in LOOP_TAGS if tags.get(tag.upper(), "") != ""]
    if present:
        details = ", ".join(f"{tag}={tags.get(tag.upper())!r}" for tag in present)
        raise UserError(f"No-loop output still has loop tag(s) for {job.output}: {details}")


def verify_job(job: TrackJob) -> None:
    if job.is_looped:
        verify_loop_job(job)
    else:
        verify_plain_job(job)


def print_job_summary(job: TrackJob) -> None:
    print(f"Track {job.track}")
    print(f"  Source      : {job.source}")
    print(f"  Output      : {job.output}")
    print(f"  Sample rate : {job.sample_rate}")
    if job.is_looped:
        print("  Mode        : LOOP")
        print(f"  LOOP_START  : {job.loop_start}  ({job.start_text})")
        print(f"  LOOP_END    : {job.loop_end}  ({job.end_text})")
        print(f"  LOOP_LENGTH : {job.loop_length}")
    else:
        print("  Mode        : NO LOOP, full file, no loop tags")


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Convert BGM tracks to Ogg Vorbis. Listed tracks are looped; optional unlisted tracks are full no-loop outputs.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Loop point file examples:

  02
  0:22.662
  1:39.195

  03 0:12.690 0:57.757

Use explicit sample positions when needed:

  04 sample:559629 sample:2547084
        """.strip(),
    )
    parser.add_argument("points_file", type=Path, help="Text/CSV file containing track id, loop start, loop end.")
    parser.add_argument("--src", type=Path, default=Path("."), help="Directory containing source audio files. Default: current directory.")
    parser.add_argument("--out", type=Path, default=Path("ogg_out"), help="Output directory. Default: ogg_out")
    parser.add_argument(
        "--source-pattern",
        help=(
            "Source filename pattern. Example: \"Track {track}.wav\" or \"{track}.wav\". "
            "Placeholders: {track}, {track_int}, {track2}, {track3}."
        ),
    )
    parser.add_argument(
        "--output-name",
        default=None,
        help=(
            "Output filename pattern. Default: {stem}.ogg. "
            "Placeholders include {track}, {track_int}, {track_index0}, {track0_2}, and {stem}. "
            "Example: --output-name \"X3_{track_index0:02d}.ogg\""
        ),
    )
    parser.add_argument(
        "--x3-pc-names",
        action="store_true",
        help=(
            "Use PC-version X3 BGM names: CD Track 01 -> X3_00.ogg, "
            "CD Track 02 -> X3_01.ogg, etc. Equivalent to --output-name \"X3_{track_index0:02d}.ogg\"."
        ),
    )
    parser.add_argument("-r", "--recursive", action="store_true", help="Search source files recursively when --source-pattern is not used.")
    parser.add_argument(
        "--include-unlisted",
        action="store_true",
        help="Also encode source audio files not listed in the points file as full-length no-loop OGGs with no loop tags.",
    )
    parser.add_argument("--sample-rate", type=int, help="Override sample rate for time-to-sample conversion. Usually not needed.")
    parser.add_argument("--quality", type=float, default=6.0, help="libvorbis quality. Default: 6.0")
    parser.add_argument("--overwrite", action="store_true", help="Overwrite existing output files.")
    parser.add_argument("--dry-run", action="store_true", help="Print what would be done without creating files.")
    parser.add_argument("--no-verify", action="store_true", help="Skip ffprobe verification after writing.")
    args = parser.parse_args(argv)

    if args.x3_pc_names:
        args.output_name = "X3_{track_index0:02d}.ogg"
    elif args.output_name is None:
        args.output_name = "{stem}.ogg"

    return args


def build_jobs(args: argparse.Namespace) -> list[TrackJob]:
    specs = parse_points_file(args.points_file)

    loop_jobs = [build_loop_job(spec, args.src, args.out, args) for spec in specs]
    jobs: list[TrackJob] = list(loop_jobs)

    if args.include_unlisted:
        loop_sources = {job.source.resolve() for job in loop_jobs}
        all_sources = sorted(
            iter_audio_files(args.src, args.recursive, skip_dir=args.out),
            key=path_sort_key,
        )
        for source in all_sources:
            if source.resolve() in loop_sources:
                continue
            # Do not treat existing OGG outputs in the source root as new sources unless the user really points src there.
            # If they are in --out, they are already skipped by skip_dir above.
            jobs.append(build_plain_job(source, args.out, args))

    # Loop jobs keep points-file order. Plain jobs are appended in source-track order.
    return jobs


def main(argv: list[str]) -> int:
    try:
        args = parse_args(argv)
        if not args.points_file.exists():
            raise UserError(f"Loop point file not found: {args.points_file}")
        if not args.src.exists() or not args.src.is_dir():
            raise UserError(f"Source directory not found: {args.src}")
        if args.sample_rate is not None and args.sample_rate <= 0:
            raise UserError("--sample-rate must be greater than zero")

        jobs = build_jobs(args)
        loop_count = sum(1 for job in jobs if job.is_looped)
        plain_count = len(jobs) - loop_count

        print(f"Found {len(jobs)} job(s): {loop_count} looped, {plain_count} no-loop.")
        for job in jobs:
            print("-" * 72)
            print_job_summary(job)
            encode_job(job, args)
            if not args.dry_run and not args.no_verify:
                verify_job(job)
                print("  Verify      : OK")

        print("-" * 72)
        if args.dry_run:
            print("Dry-run complete. No files were written.")
        else:
            print("Done.")
        return 0
    except UserError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
