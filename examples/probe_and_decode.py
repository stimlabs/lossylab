"""Probe and decode every file under a directory in a pool of worker processes.

Writes one JSON line per file to the output file and logs each failure and a summary of the outcomes:

    uv run python examples/probe_and_decode.py path/to/files --output-file probe_and_decode.jsonl

A file that fails to probe or decode gets a FileError in its line instead of stopping the run. A worker process that
dies (a segfault, an abort, being killed for memory) breaks the pool: the script then logs the files that did not
finish and exits with status 1. One of them killed its worker; with --num-workers 1 it is the first one listed.
"""

import argparse
import collections
import json
import logging
import sys
from concurrent.futures import ProcessPoolExecutor, as_completed
from concurrent.futures.process import BrokenProcessPool
from pathlib import Path

import lossylab

logger = logging.getLogger("probe_and_decode")


def probe_and_decode_file(path: Path) -> dict:
    """The probe and, if it succeeded, the decode of one file, each as {"ok": ..., "value" or "error": ..., "log"}."""
    source = lossylab.Source.from_path(str(path))
    line = {"path": str(path)}

    probed = lossylab.capture_probe(source)
    line["probe"] = probed.to_dict()
    if not probed:
        return line

    decoded = lossylab.capture_decode_image(source)
    if decoded:
        line["decode"] = {"ok": True, "value": decoded.value().record.to_dict()}
    else:
        line["decode"] = {"ok": False, "error": decoded.error().to_dict()}
    line["decode"]["log"] = [message.to_dict() for message in decoded.log()]
    return line


def outcomes(line: dict) -> list[str]:
    """One label per operation, such as "probe: ok" or "decode: FFmpeg"."""
    labels = []
    for operation in ("probe", "decode"):
        if operation in line:
            result = line[operation]
            labels.append(f"{operation}: " + ("ok" if result["ok"] else result["error"]["kind"]))
    return labels


def main() -> int:
    parser = argparse.ArgumentParser(description="Probe and decode every file under a directory.")
    parser.add_argument("directory", type=Path)
    parser.add_argument("--output-file", type=Path, default=Path("probe_and_decode.jsonl"), help="JSON lines, one per file")
    parser.add_argument("--num-workers", type=int, default=4, help="worker processes (default: 4)")
    args = parser.parse_args()
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")

    paths = sorted(path for path in args.directory.rglob("*") if path.is_file())
    logger.info("probing and decoding %d files under %s with %d workers", len(paths), args.directory,
                args.num_workers)
    counts = collections.Counter()

    with ProcessPoolExecutor(max_workers=args.num_workers) as executor, args.output_file.open("w") as output:
        futures = {executor.submit(probe_and_decode_file, path): path for path in paths}
        try:
            for future in as_completed(futures):
                line = future.result()
                output.write(json.dumps(line) + "\n")
                counts.update(outcomes(line))
                for operation in ("probe", "decode"):
                    result = line.get(operation)
                    if result is not None and not result["ok"]:
                        logger.warning("%s: %s failed (%s): %s", line["path"], operation, result["error"]["kind"],
                                       result["error"]["message"])
        except BrokenProcessPool:
            unfinished = sorted(str(path) for future, path in futures.items() if future.exception() is not None)
            logger.error("a worker process died; %d files did not finish:\n  %s", len(unfinished),
                         "\n  ".join(unfinished))
            return 1

    logger.info("wrote %d lines to %s", len(paths), args.output_file)
    for label, count in sorted(counts.items()):
        logger.info("  %s: %d", label, count)
    return 0


if __name__ == "__main__":
    sys.exit(main())
