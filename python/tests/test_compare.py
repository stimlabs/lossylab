import math
from pathlib import Path

import numpy as np
import pytest

import lossylab

DATA_DIR = Path(__file__).resolve().parents[2] / "tests" / "data"


def yuv444_frame(luma):
    height, width = luma.shape
    frame = lossylab.Frame.allocate(
        width, height, lossylab.PixelFormat.from_name("yuv444p"), lossylab.ColorSpec.bt709_limited()
    )
    for index in range(frame.plane_count()):
        frame.writable_plane(index)[...] = luma if index == 0 else 128
    return frame


def test_psnr_and_ssim_are_reported_per_frame_and_pooled():
    reference = yuv444_frame(np.full((48, 64), 100, dtype=np.uint8))
    distorted = yuv444_frame(np.full((48, 64), 104, dtype=np.uint8))

    result = lossylab.compare(reference, distorted, [lossylab.Metric.Psnr, lossylab.Metric.Ssim])
    values = result.frames[0]
    assert values["mse_y"] == pytest.approx(16.0)
    assert values["psnr_y"] == pytest.approx(10 * math.log10(255**2 / 16))
    assert 0 < values["ssim"] <= 1
    assert result.pooled["psnr_mean"] == values["psnr"]
    assert result.record.kind == lossylab.StageKind.Compare
    assert result.configuration["metrics"] == ["psnr", "ssim"]


def test_identical_frames_have_infinite_psnr():
    frame = yuv444_frame(np.arange(64 * 48, dtype=np.uint8).reshape(48, 64))
    assert math.isinf(lossylab.compare(frame, frame, [lossylab.Metric.Psnr]).frames[0]["psnr"])


def test_capture_compare_returns_a_refusal_as_a_file_error():
    source = lossylab.Source.from_path(str(DATA_DIR / "testsrc_64x48.png"))
    decoded = lossylab.decode_image(source).frame

    refused = lossylab.capture_compare(source, [decoded], [decoded], [lossylab.Metric.Psnr])
    assert not refused
    assert refused.error().kind == lossylab.FileErrorKind.ConversionRefused
    assert refused.error().operation == "compare"

    options = lossylab.CompareOptions()
    options.strict = lossylab.Strict.AllowRecorded
    compared = lossylab.capture_compare(source, [decoded], [decoded], [lossylab.Metric.Psnr], options)
    assert compared
    assert compared.value().record.params["measured_as"]["psnr"] == "gbrp"
