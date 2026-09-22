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


def test_probe_attributes_that_were_dict_only():
    result = lossylab.probe(lossylab.Source.from_path(str(DATA_DIR / "testsrc_64x48.mp4")))
    assert result.claimed_extension == "mp4"
    assert result.format_mismatch is False
    assert result.compatible_brands == ["isom", "iso2", "avc1", "mp41"]
    stream = result.primary_video_stream()
    assert stream.is_variable_frame_rate is False
    assert stream.image_container is None


def test_a_renamed_file_is_a_format_mismatch(tmp_path):
    renamed = tmp_path / "picture.mp4"
    renamed.write_bytes((DATA_DIR / "testsrc_64x48.png").read_bytes())
    result = lossylab.probe(lossylab.Source.from_path(str(renamed)))
    assert result.claimed_extension == "mp4"
    assert result.format_mismatch is True


@pytest.mark.parametrize(
    ("fixture", "compression", "has_alpha"),
    [
        ("testsrc_64x48_lossy.webp", "lossy", False),
        ("testsrc_64x48_lossless.webp", "lossless", False),
        ("testsrc_64x48_lossy_alpha.webp", "lossy", True),
        ("testsrc_64x48_lossless_alpha.webp", "lossless", True),
    ],
)
def test_webp_container_properties(fixture, compression, has_alpha):
    stream = lossylab.probe(lossylab.Source.from_path(str(DATA_DIR / fixture))).primary_video_stream()
    info = stream.image_container
    assert info.compression == compression
    assert info.has_alpha is has_alpha
    assert info.is_animated is False
    assert info.frame_count == 1
    assert stream.to_dict()["image_container"]["compression"] == compression


def test_an_animated_webp_reports_its_frames_and_canvas():
    info = lossylab.probe(
        lossylab.Source.from_path(str(DATA_DIR / "testsrc_64x48_animated.webp"))
    ).primary_video_stream().image_container
    assert info.is_animated is True
    assert info.frame_count == 2
    assert (info.canvas_width, info.canvas_height) == (64, 48)


def test_to_dict_round_trips_through_json():
    source = lossylab.Source.from_path(str(DATA_DIR / "testsrc_64x48.png"))
    document = lossylab.probe(source).to_dict()
    assert json.loads(json.dumps(document)) == document
