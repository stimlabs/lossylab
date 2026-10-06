from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

import pytest

import lossylab

DATA_DIR = Path(__file__).resolve().parents[2] / "tests" / "data"


def read_fixture(name, options=None):
    source = lossylab.Source.from_path(str(DATA_DIR / name))
    if options is None:
        return lossylab.read_headers(source)
    return lossylab.read_headers(source, options)


def test_h264_headers_are_recovered():
    info = read_fixture("testsrc_64x48.mp4")
    assert info.codec_name == "h264"

    sps = next(parameter_set for parameter_set in info.parameter_sets if parameter_set.kind == "sps")
    assert sps.fields["profile_idc"] == 100

    assert len(info.slices) == 5
    assert info.slices[0].slice_type == "I"
    assert all(slice_info.qp is not None for slice_info in info.slices)

    assert info.embedded_encoder_settings_availability == lossylab.Availability.Present
    assert info.embedded_encoder_settings.startswith("x264 - core")
    assert info.encoder_settings["cabac"] == "1"
    assert info.bitstream_color["matrix"] == lossylab.ColorSpec.bt709_limited().to_dict()["matrix"]


def test_vp9_quantizer_indices_are_recovered():
    info = read_fixture("testsrc_64x48_vp9.webm")
    assert len(info.quantizer_indices) == 5
    assert info.embedded_encoder_settings is None


def test_max_slices_limits_the_read():
    options = lossylab.ReadHeadersOptions()
    options.max_slices = 2
    assert len(read_fixture("testsrc_64x48.mp4", options).slices) == 2


def test_to_dict_carries_the_schema_version():
    as_dict = read_fixture("testsrc_64x48_hevc.mp4").to_dict()
    assert as_dict["codec_name"] == "hevc"
    assert "schema_version" in as_dict


def test_an_uninterpreted_codec_raises_unsupported_capability():
    with pytest.raises(lossylab.UnsupportedCapability, match=r"read_headers\(\) does not interpret 'png'"):
        read_fixture("testsrc_64x48.png")


def test_trace_lines_do_not_reach_a_python_log_handler():
    messages = []
    lossylab.set_log_handler(messages.append, lossylab.LogLevel.Trace)
    try:
        read_fixture("testsrc_64x48.mp4")
    finally:
        lossylab.set_log_handler(None)
    assert not any("profile_idc" in message.text for message in messages)


def test_threads_read_headers_in_parallel():
    names = ["testsrc_64x48.mp4", "testsrc_64x48_hevc.mp4"] * 4
    expected = {name: read_fixture(name).to_dict() for name in set(names)}
    with ThreadPoolExecutor(max_workers=4) as pool:
        results = list(pool.map(lambda name: read_fixture(name).to_dict(), names))
    for name, result in zip(names, results):
        assert result == expected[name]


def test_capture_read_headers_returns_a_file_error_for_unsupported_codecs():
    result = lossylab.capture_read_headers(lossylab.Source.from_path(str(DATA_DIR / "testsrc_64x48.png")))
    assert not result.ok()
    assert result.error().kind == lossylab.FileErrorKind.UnsupportedCapability
    assert result.error().details["name"] == "png"
    assert result.to_dict()["ok"] is False
