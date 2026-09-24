"""Pilot audit: probe every file under a directory, and decode, measure and trace the compression history of the still
images among them.

Writes one JSON line per file to the output file. Each line has the file's "path", its "group", and, per stage that
ran, an entry of the form {"ok": ..., "value" or "error": ..., "log": [...]}, where "log" is what FFmpeg logged during
that stage:
  - probe: container, streams, color, JPEG markers, ICC profile, encoder fingerprints
  - decode: the decode's record (still images only); the image is decoded as the file tags its color
  - measure: signal levels, blockiness, blurriness, noise, letterbox
  - compression_history: traces of earlier lossy compression (compression_history() with its defaults): JPEG
    quantization tables, from a JPEG file's header or read off the pixels of any other file, with the libjpeg quality
    and chroma subsampling they imply; chroma upsampled from 4:2:0, 4:2:2 or 4:4:0; and a WebP recompression curve

Run as:

    uv run python examples/pilot_audit.py path/to/files --output-file pilot_audit.jsonl

or, to audit 20 randomly picked JPEG, PNG and WebP files from each directory directly under path/to/files:

    uv run python examples/pilot_audit.py path/to/files --group-depth 1 --files-per-group 20 --pattern jpg png webp

File selection: with --group-depth N, each directory exactly N levels below the given directory is a group, holding
every file anywhere below it; files above that depth belong to no group and are skipped. Without it, the whole tree is
one group, ".". --files-per-group picks that many files from each group at random. A group's pick depends only on
--seed and the group's relative path, so adding or removing another group does not change it. --pattern takes
one or more globs or bare file endings ("jpg" or ".jpg" stands for "*.jpg") and matches file names case-insensitively,
so "*.jpg" also matches "IMG_0001.JPG". Since it takes every argument that follows it, give the directory before
--pattern. An entry whose name has one of the given file endings is taken to be a file without checking its type,
so a directory named like an image is treated as a file. --listing-workers directories are listed at a time, across all groups.

Files are audited on a pool of threads; lossylab releases the GIL for every call. A library call that fails produces
a FileError in the file's line instead of stopping the run. A Python exception while auditing a file is logged, written
as that file's line under "exception", and makes the script exit with status 1. A crash in native code (a segfault or
abort) ends the whole process; the lines written up to then remain in the output file.

Measurement caveats:
  - Blockiness, blurriness, and noise are verified on synthetic frames, not real images.
    Expect false positives on screenshots and graphics.
  - compression_history() recovers libjpeg quality 30 to 95 and the chroma subsampling of JPEGs saved losslessly, also
    after a crop, and WebP quality to within a few steps on detailed content; see its documentation for the limits.
    Nothing survives a resize after the last compression, and in a file that is not itself a JPEG, JPEG qualities
    above about 95 leave no lattice to find.
  - JPEG quantization table recognition is limited to libjpeg. Other encoders (camera
    firmware, Photoshop, FFmpeg, mozjpeg) report "not exact libjpeg" with a quality estimate.
"""

from __future__ import annotations

import argparse
import collections
import dataclasses
import fnmatch
import json
import logging
import os
import random
import sys
from concurrent.futures import FIRST_COMPLETED, ThreadPoolExecutor, as_completed, wait
from pathlib import Path

import lossylab

logger = logging.getLogger("pilot_audit")

STAGES = ("probe", "decode", "measure", "compression_history")

ANALYZERS = [
    lossylab.Analyzer.SignalLevels,
    lossylab.Analyzer.Blockiness,
    lossylab.Analyzer.Blurriness,
    lossylab.Analyzer.Noise,
    lossylab.Analyzer.Letterbox,
]


@dataclasses.dataclass(frozen=True)
class NamePatterns:
    """--pattern, split into file endings such as ".jpg" and the remaining globs, all lowercase. A name with one of
    the file endings is taken to be a file without checking its type."""

    file_endings: tuple[str, ...]
    globs: tuple[str, ...]

    @classmethod
    def parse(cls, patterns: list[str]) -> NamePatterns:
        """Reads "jpg", ".jpg" and "*.jpg" as the file ending ".jpg", and anything else as a glob."""
        file_endings = []
        globs = []
        for pattern in patterns:
            pattern = pattern.lower()
            ending = pattern[2:] if pattern.startswith("*.") else pattern.removeprefix(".")
            if ending and not any(character in ending for character in "*?["):
                file_endings.append("." + ending)
            else:
                globs.append(pattern)
        return cls(tuple(file_endings), tuple(globs))

    def names_file(self, lowercase_name: str) -> bool:
        return lowercase_name.endswith(self.file_endings)

    def matches(self, lowercase_name: str) -> bool:
        return self.names_file(lowercase_name) or any(fnmatch.fnmatchcase(lowercase_name, glob) for glob in self.globs)


def log_listing_error(error: OSError) -> None:
    logger.warning("cannot list %s: %s", error.filename, error.strerror)


def scan_directory(directory: Path, name_patterns: NamePatterns) -> tuple[list[Path], list[Path]]:
    """The matching files and the subdirectories directly in `directory`."""
    matching_paths = []
    subdirectories = []
    try:
        with os.scandir(directory) as entries:
            for entry in entries:
                lowercase_name = entry.name.lower()
                if name_patterns.names_file(lowercase_name):
                    matching_paths.append(Path(entry.path))
                elif entry.is_dir(follow_symlinks=False):
                    subdirectories.append(Path(entry.path))
                elif name_patterns.matches(lowercase_name):
                    matching_paths.append(Path(entry.path))
    except OSError as error:
        log_listing_error(error)
    return matching_paths, subdirectories


def find_groups(directory: Path, group_depth: int, name_patterns: NamePatterns) -> tuple[list[str], int]:
    """The directories `group_depth` levels below `directory`, relative to it, and the number of matching files
    above them."""
    groups = [""]
    num_ungrouped = 0
    for _ in range(group_depth):
        subdirectories = []
        for group in groups:
            matching_paths, group_subdirectories = scan_directory(directory / group, name_patterns)
            num_ungrouped += len(matching_paths)
            subdirectories.extend(
                f"{group}/{subdirectory.name}" if group else subdirectory.name for subdirectory in group_subdirectories
            )
        groups = subdirectories
    return sorted(groups), num_ungrouped


def list_groups(
    directory: Path, groups: list[str], name_patterns: NamePatterns, listing_workers: int
) -> list[list[Path]]:
    """The matching files below each group, listing the directories of all groups on one pool of threads."""
    paths_by_group: list[list[Path]] = [[] for _ in groups]
    with ThreadPoolExecutor(max_workers=listing_workers) as executor:
        pending = {
            executor.submit(scan_directory, directory / group, name_patterns): index
            for index, group in enumerate(groups)
        }
        while pending:
            done, _ = wait(pending, return_when=FIRST_COMPLETED)
            for future in done:
                index = pending.pop(future)
                matching_paths, subdirectories = future.result()
                paths_by_group[index].extend(matching_paths)
                for subdirectory in subdirectories:
                    pending[executor.submit(scan_directory, subdirectory, name_patterns)] = index
    return paths_by_group


def select_files(
    directory: Path,
    group_depth: int | None,
    files_per_group: int | None,
    seed: int,
    patterns: list[str],
    listing_workers: int,
) -> list[tuple[Path, str]]:
    """The files to audit, each with its group's path relative to `directory`, sorted by group and then path."""
    name_patterns = NamePatterns.parse(patterns)
    if group_depth is None:
        groups = ["."]
    else:
        groups, num_ungrouped = find_groups(directory, group_depth, name_patterns)
        if num_ungrouped:
            logger.warning("skipped %d matching files above group depth %d", num_ungrouped, group_depth)
    logger.info("listing %d groups under %s", len(groups), directory)

    selected = []
    for group, group_paths in zip(groups, list_groups(directory, groups, name_patterns, listing_workers)):
        group_paths = sorted(group_paths)
        if not group_paths:
            logger.warning("group %s has no matching files", group)
            continue
        if files_per_group is not None:
            if len(group_paths) <= files_per_group:
                logger.info("group %s has %d matching files; taking all of them", group, len(group_paths))
            else:
                group_paths = sorted(random.Random(f"{seed}:{group}").sample(group_paths, files_per_group))
        selected.extend((path, group) for path in group_paths)
    return selected


def audit_file(path: Path, group: str) -> dict:
    """The probe of one file and, for a still image, its decode, measurement and compression history."""
    source = lossylab.Source.from_path(str(path))
    line: dict = {"path": str(path), "group": group}

    probe_result = lossylab.capture_probe(source)
    line["probe"] = probe_result.to_dict()
    if probe_result and is_still_image(probe_result.value()):
        audit_still_image(source, line)
    return line


def is_still_image(probe: lossylab.ProbeResult) -> bool:
    """True when the file's primary picture is a single image rather than a video or an animation."""
    if probe.primary_tile_grid() is not None:
        return True
    stream = probe.primary_video_stream()
    if stream is None:
        return False
    if stream.image_container is not None:
        return not stream.image_container.is_animated
    return probe.format_name == "image2" or probe.format_name.endswith("_pipe")


def audit_still_image(source: lossylab.Source, line: dict) -> None:
    """Adds the decode, measurement and compression history of a still image to its line."""
    decode_result = lossylab.capture_decode_image(source)
    line["decode"] = decode_result_to_dict(decode_result)
    if not decode_result:
        return
    decoded_image = decode_result.value()

    measure_options = lossylab.MeasureOptions()
    measure_options.strict = lossylab.Strict.AllowRecorded
    measure_result = lossylab.capture_measure(source, [decoded_image.frame], ANALYZERS, measure_options)
    line["measure"] = measure_result.to_dict()

    compression_history_result = lossylab.capture_compression_history(source, decoded_image)
    line["compression_history"] = compression_history_result.to_dict()


def decode_result_to_dict(decode_result: lossylab.DecodeImageFileResult) -> dict:
    """A decode's outcome in the shape of FileResult.to_dict(), with the decode's record as its value."""
    if decode_result:
        entry = {"ok": True, "value": decode_result.value().record.to_dict()}
    else:
        entry = {"ok": False, "error": decode_result.error().to_dict()}
    entry["log"] = [message.to_dict() for message in decode_result.log()]
    return entry


def outcomes(line: dict) -> list[str]:
    """One label per stage that ran, such as "probe: ok" or "decode: FFmpeg"."""
    labels = []
    for stage in STAGES:
        if stage in line:
            entry = line[stage]
            labels.append(f"{stage}: " + ("ok" if entry["ok"] else entry["error"]["kind"]))
    return labels


def log_failures(line: dict) -> None:
    for stage in STAGES:
        entry = line.get(stage)
        if entry is not None and not entry["ok"]:
            logger.warning(
                "%s: %s failed (%s): %s", line["path"], stage, entry["error"]["kind"], entry["error"]["message"]
            )


def positive_int(text: str) -> int:
    value = int(text)
    if value < 1:
        raise argparse.ArgumentTypeError(f"must be at least 1, got {value}")
    return value


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Pilot audit: probe every file under a directory; decode, measure and trace still images.",
        epilog=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("directory", type=Path, help="directory to scan recursively")
    parser.add_argument("--output-file", type=Path, default=Path("pilot_audit.jsonl"), help="JSON lines, one per file")
    parser.add_argument("--num-workers", type=positive_int, default=4, help="worker threads (default: 4)")
    parser.add_argument(
        "--group-depth",
        type=positive_int,
        help="treat each directory this many levels below DIRECTORY as a group (default: the whole tree is one group)",
    )
    parser.add_argument(
        "--files-per-group", type=positive_int, help="randomly pick this many files from each group (default: all)"
    )
    parser.add_argument("--seed", type=int, default=0, help="seed for --files-per-group (default: 0)")
    parser.add_argument(
        "--pattern",
        nargs="+",
        default=["*"],
        help="globs or file endings a file name must match one of, case-insensitively, e.g. jpg png (default: *)",
    )
    parser.add_argument(
        "--listing-workers",
        type=positive_int,
        default=16,
        help="threads listing directories in parallel (default: 16)",
    )
    args = parser.parse_args()
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")

    selected = select_files(
        args.directory, args.group_depth, args.files_per_group, args.seed, args.pattern, args.listing_workers
    )
    logger.info(
        "auditing %d files in %d groups under %s with %d workers; writing to %s",
        len(selected),
        len({group for _, group in selected}),
        args.directory,
        args.num_workers,
        args.output_file,
    )
    counts = collections.Counter()
    num_exceptions = 0

    with ThreadPoolExecutor(max_workers=args.num_workers) as executor, args.output_file.open("w") as output:
        futures = {executor.submit(audit_file, path, group): (path, group) for path, group in selected}
        for future in as_completed(futures):
            path, group = futures[future]
            try:
                line = future.result()
            except Exception as exception:
                logger.exception("%s: exception while auditing", path)
                line = {"path": str(path), "group": group, "exception": repr(exception)}
                num_exceptions += 1
            output.write(json.dumps(line) + "\n")
            output.flush()
            counts.update(outcomes(line))
            log_failures(line)

    logger.info("wrote %d lines to %s", len(selected), args.output_file)
    logger.info("outcome summary:")
    for label, count in sorted(counts.items()):
        logger.info("  %s: %d", label, count)
    if num_exceptions:
        logger.error("%d files raised an exception while auditing", num_exceptions)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
