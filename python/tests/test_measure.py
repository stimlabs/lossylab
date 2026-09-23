from pathlib import Path

import numpy as np
import pytest

import lossylab

DATA_DIR = Path(__file__).resolve().parents[2] / "tests" / "data"

IMAGE_ANALYZERS = [
    lossylab.Analyzer.SignalLevels,
    lossylab.Analyzer.Blockiness,
    lossylab.Analyzer.Blurriness,
    lossylab.Analyzer.Noise,
    lossylab.Analyzer.Letterbox,
]


def luma_frame(luma):
    height, width = luma.shape
    frame = lossylab.Frame.allocate(
        width, height, lossylab.PixelFormat.from_name("yuv420p"), lossylab.ColorSpec.bt709_limited()
    )
    for index in range(frame.plane_count()):
        frame.writable_plane(index)[...] = luma if index == 0 else 128
    return frame


def test_a_decoded_jpeg_gets_every_image_measurement():
    decoded = lossylab.decode_image(lossylab.Source.from_path(str(DATA_DIR / "testsrc_64x48_q75.jpg")))
    result = lossylab.measure(decoded.frame, IMAGE_ANALYZERS)

    values = result.frames[0].values
    for name in ("luma_mean", "outside_limited_range", "blockiness", "blurriness", "noise_sigma", "content_fraction"):
        assert name in values
    assert result.record.kind == lossylab.StageKind.Measure
    assert result.record.params["measured_as"]["signal_levels"] == "yuvj420p"
    assert result.to_dict()["frames"][0]["values"]["luma_mean"] == values["luma_mean"]


def test_noise_recovers_the_sigma_of_added_noise():
    rng = np.random.default_rng(5)
    luma = np.clip(128 + rng.normal(0, 4, (128, 128)), 0, 255).round().astype(np.uint8)
    result = lossylab.measure(luma_frame(luma), [lossylab.Analyzer.Noise])
    assert result.frames[0].value("noise_sigma") == pytest.approx(4.0, abs=0.4)


def test_letterbox_reports_the_content_rectangle():
    luma = np.full((48, 64), 16, dtype=np.uint8)
    luma[8:40, :] = 150
    result = lossylab.measure(luma_frame(luma), [lossylab.Analyzer.Letterbox])
    rect = result.frames[0].content_rect
    assert (rect.x, rect.y, rect.width, rect.height) == (0, 8, 64, 32)
    assert result.frames[0].values["letterbox_top"] == 8


def test_an_rgb_frame_is_refused_unless_conversion_is_allowed():
    decoded = lossylab.decode_image(lossylab.Source.from_path(str(DATA_DIR / "testsrc_64x48.png")))
    with pytest.raises(lossylab.ConversionRefused):
        lossylab.measure(decoded.frame, [lossylab.Analyzer.SignalLevels])

    options = lossylab.MeasureOptions()
    options.strict = lossylab.Strict.AllowRecorded
    result = lossylab.measure(decoded.frame, [lossylab.Analyzer.SignalLevels], options)
    assert result.record.params["measured_as"]["signal_levels"] == "yuv444p"
    assert result.record.conversions


def test_capture_measure_returns_a_refusal_as_a_file_error():
    source = lossylab.Source.from_path(str(DATA_DIR / "testsrc_64x48.png"))
    decoded = lossylab.decode_image(source)

    refused = lossylab.capture_measure(source, [decoded.frame], [lossylab.Analyzer.SignalLevels])
    assert not refused
    assert refused.error().kind == lossylab.FileErrorKind.ConversionRefused
    assert refused.to_dict()["error"]["operation"] == "measure"

    measured = lossylab.capture_measure(source, [decoded.frame], [lossylab.Analyzer.Letterbox])
    assert measured
    assert measured.value().frames[0].values["content_fraction"] == 1.0
