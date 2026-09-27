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

    frame = result.evidence.frames[0]
    assert frame.signal_levels.luma.mean > 0
    assert 0 <= frame.signal_levels.outside_limited_range <= 1
    assert frame.blockiness is not None
    assert frame.blurriness is not None
    assert frame.noise_sigma is not None
    assert frame.letterbox.content_fraction > 0
    assert result.record.kind == lossylab.StageKind.Measure
    assert result.evidence.measured_as["signal_levels"] == "yuvj420p"
    assert result.configuration.analyzers == IMAGE_ANALYZERS

    # The measurements are in the record, so a stored record holds them.
    stored = result.to_dict()["record"]["evidence"]
    assert stored["frames"][0]["signal_levels"]["luma"]["mean"] == frame.signal_levels.luma.mean


def test_pooled_measurements_are_summaries_across_frames():
    result = lossylab.measure([luma_frame(np.full((48, 64), level, dtype=np.uint8)) for level in (60, 100, 140)],
                              [lossylab.Analyzer.SignalLevels])
    luma_mean = result.evidence.pooled["signal_levels.luma.mean"]
    assert (luma_mean.count, luma_mean.mean, luma_mean.median) == (3, 100.0, 100.0)
    assert (luma_mean.minimum, luma_mean.maximum, luma_mean.std) == (60.0, 140.0, 40.0)
    assert result.to_dict()["record"]["evidence"]["pooled"]["signal_levels.luma.mean"]["std"] == 40.0
    assert "index" not in result.evidence.pooled


def test_noise_recovers_the_sigma_of_added_noise():
    rng = np.random.default_rng(5)
    luma = np.clip(128 + rng.normal(0, 4, (128, 128)), 0, 255).round().astype(np.uint8)
    result = lossylab.measure(luma_frame(luma), [lossylab.Analyzer.Noise])
    assert result.evidence.frames[0].noise_sigma == pytest.approx(4.0, abs=0.4)


def test_letterbox_reports_the_content_rectangle():
    luma = np.full((48, 64), 16, dtype=np.uint8)
    luma[8:40, :] = 150
    result = lossylab.measure(luma_frame(luma), [lossylab.Analyzer.Letterbox])
    letterbox = result.evidence.frames[0].letterbox
    rect = letterbox.content_rect
    assert (rect.x, rect.y, rect.width, rect.height) == (0, 8, 64, 32)
    assert letterbox.bars.top == 8


def test_an_rgb_frame_is_refused_unless_conversion_is_allowed():
    decoded = lossylab.decode_image(lossylab.Source.from_path(str(DATA_DIR / "testsrc_64x48.png")))
    with pytest.raises(lossylab.ConversionRefused):
        lossylab.measure(decoded.frame, [lossylab.Analyzer.SignalLevels])

    options = lossylab.MeasureOptions()
    options.analyzers = [lossylab.Analyzer.SignalLevels]
    options.strict = lossylab.Strict.AllowRecorded
    result = lossylab.measure(decoded.frame, options)
    assert result.evidence.measured_as["signal_levels"] == "yuv444p"
    assert result.record.conversions

    with pytest.raises(lossylab.ConfigError):
        lossylab.measure(decoded.frame, lossylab.MeasureOptions())


def test_capture_measure_returns_a_refusal_as_a_file_error():
    source = lossylab.Source.from_path(str(DATA_DIR / "testsrc_64x48.png"))
    decoded = lossylab.decode_image(source)

    refused = lossylab.capture_measure(source, [decoded.frame], [lossylab.Analyzer.SignalLevels])
    assert not refused
    assert refused.error().kind == lossylab.FileErrorKind.ConversionRefused
    assert refused.to_dict()["error"]["operation"] == "measure"

    measured = lossylab.capture_measure(source, [decoded.frame], [lossylab.Analyzer.Letterbox])
    assert measured
    assert measured.value().evidence.frames[0].letterbox.content_fraction == 1.0
