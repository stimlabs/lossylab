"""Equalize files from per-file steps and store the results as RGB arrays.

Reads a JSON Lines file with one object per file:

    {"path": "a/b.jpg", "crop": [16, 16, 1024, 768], "orientation": 6, "achromatic": false,
     "jpeg_pixel_format": "yuvj420p"}

- "crop": x, y, width and height of the rectangle to keep, in the file's stored orientation; null keeps everything.
- "orientation": the EXIF orientation (1 to 8) to turn the image upright from.
- "achromatic": true replaces every pixel by its BT.601 luma.
- "jpeg_pixel_format": the subsampling of a JPEG at qscale 2 the image goes through ("yuvj420p", "yuvj422p" or
  "yuvj444p"); null for none.

Every file is decoded to sRGB rgb24 (ICC profiles converted, alpha over black, swscale's bicubic chroma upsampler).
Each result is written to the output directory as a uint8 array of shape (height, width, 3), in a .npy file named by
the file's SHA-256. records.jsonl in the same directory gets one line per file: its path, its spec and its
ProcessingRecord, or the error when it failed.

Run as:

    uv run python examples/equalize.py steps.jsonl --output-dir equalized --workers 8
"""

import argparse
import json
import logging
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

import numpy

import lossylab

logger = logging.getLogger(__name__)


def to_srgb24():
    options = lossylab.ConvertOptions()
    options.pixel_format = lossylab.PixelFormat.from_name("rgb24")
    options.color = lossylab.ColorSpec.srgb()
    options.chroma_up = lossylab.KernelSpec()
    options.chroma_up.kernel = lossylab.Kernel.Bicubic
    return options


def jpeg_color():
    """Full-range BT.601 YCbCr, with sRGB's primaries and transfer, so the encode keeps the colors."""
    color = lossylab.ColorSpec.jpeg()
    color.primaries = lossylab.ColorPrimaries.Bt709
    color.transfer = lossylab.TransferCharacteristic.Srgb
    return color


def spec_for(steps):
    """The pipeline one line of the steps file describes."""
    decode = lossylab.DecodeImageOptions()
    decode.conversion = to_srgb24()
    spec = lossylab.PipelineSpec()
    spec.add(decode)

    if steps.get("crop") is not None:
        crop = lossylab.CropOptions()
        crop.x, crop.y, crop.width, crop.height = steps["crop"]
        spec.add(crop)
    if steps.get("orientation", 1) != 1:
        orient = lossylab.OrientOptions()
        orient.orientation = steps["orientation"]
        spec.add(orient)
    if steps.get("achromatic"):
        spec.add(lossylab.AchromaticOptions())
    if steps.get("jpeg_pixel_format") is not None:
        jpeg = lossylab.RoundtripImageConfiguration()
        jpeg.encode.codec = lossylab.ImageCodec.Mjpeg
        jpeg.encode.pixel_format = lossylab.PixelFormat.from_name(steps["jpeg_pixel_format"])
        jpeg.encode.color = jpeg_color()
        jpeg.encode.rate_control = lossylab.RateControl.quality(2)
        jpeg.encode.strict = lossylab.Strict.AllowRecorded
        jpeg.decode.conversion = to_srgb24()
        spec.add(jpeg)
    return spec


def equalize(steps):
    spec = spec_for(steps)
    return steps["path"], spec, lossylab.capture_run(lossylab.Source.from_path(steps["path"]), lossylab.Pipeline(spec))


def main():
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("steps", type=Path, help="JSON Lines file with one object per file")
    parser.add_argument("--output-dir", type=Path, required=True, help="directory to write the arrays and records to")
    parser.add_argument("--workers", type=int, default=4, help="files equalized at a time")
    arguments = parser.parse_args()

    with arguments.steps.open() as lines:
        all_steps = [json.loads(line) for line in lines if line.strip()]
    arguments.output_dir.mkdir(parents=True, exist_ok=True)

    written = failed = 0
    with (
        ThreadPoolExecutor(arguments.workers) as pool,
        (arguments.output_dir / "records.jsonl").open("a") as records,
    ):
        for path, spec, result in pool.map(equalize, all_steps):
            line = {"path": path, "spec": spec.to_dict()}
            if result:
                record = result.value().record
                name = record.stages()[0].evidence.source_sha256.removeprefix("sha256:") + ".npy"
                numpy.save(arguments.output_dir / name, result.value().frame.to_numpy())
                line["array"] = name
                line["record"] = record.to_dict()
                written += 1
            else:
                logger.warning("%s: %s", path, result.error().message)
                line["error"] = result.error().to_dict()
                failed += 1
            records.write(json.dumps(line) + "\n")
    logger.info("%d files written to %s, %d failed", written, arguments.output_dir, failed)


if __name__ == "__main__":
    main()
