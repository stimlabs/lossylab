import json
from pathlib import Path

import pytest

import lossylab

DATA_DIR = Path(__file__).resolve().parents[2] / "tests" / "data"


def test_probe_image():
    source = lossylab.Source.from_path(str(DATA_DIR / "testsrc_64x48.png"))
    result = lossylab.probe(source)
    stream = result.primary_video_stream()
    assert stream is not None
    assert stream.width == 64
    assert stream.height == 48


def test_probe_video():
    source = lossylab.Source.from_path(str(DATA_DIR / "testsrc_64x48.mp4"))
    result = lossylab.probe(source)
    stream = result.primary_video_stream()
    assert stream is not None
    assert stream.codec_name
    assert stream.frame_rate.is_valid()


def test_probe_from_bytes():
    data = (DATA_DIR / "testsrc_64x48.png").read_bytes()
    source = lossylab.Source.from_bytes(data)
    result = lossylab.probe(source)
    assert result.primary_video_stream() is not None


def test_probe_from_memory_keeps_buffer_alive():
    data = (DATA_DIR / "testsrc_64x48.png").read_bytes()
    source = lossylab.Source.from_memory(data)
    del data  # Source.from_memory borrows; it must hold its own reference.
    result = lossylab.probe(source)
    assert result.primary_video_stream() is not None


def test_probe_nonexistent_path_raises_error():
    with pytest.raises(lossylab.Error):
        lossylab.probe(lossylab.Source.from_path("/nonexistent/path/does-not-exist.mp4"))


def test_probe_garbage_bytes_raises_error():
    with pytest.raises(lossylab.Error):
        lossylab.probe(lossylab.Source.from_memory(b"not a real container"))


def test_empty_path_raises_config_error():
    with pytest.raises(lossylab.ConfigError):
        lossylab.Source.from_path("")


def test_probe_empty_memory_raises_config_error():
    with pytest.raises(lossylab.ConfigError):
        lossylab.probe(lossylab.Source.from_memory(b""))


def test_to_dict_round_trips_through_json():
    source = lossylab.Source.from_path(str(DATA_DIR / "testsrc_64x48.png"))
    document = lossylab.probe(source).to_dict()
    assert json.loads(json.dumps(document)) == document
