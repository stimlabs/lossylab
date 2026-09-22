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


def test_a_grid_avif_reports_its_tile_grid():
    result = lossylab.probe(lossylab.Source.from_path(str(DATA_DIR / "testsrc_128x96_grid_alpha.avif")))
    assert result.primary_video_stream() is None

    grid = result.primary_tile_grid()
    assert (grid.width, grid.height) == (128, 96)
    assert grid.title == "Color"
    assert sorted((tile.x, tile.y) for tile in grid.tiles) == [(0, 0), (0, 64), (64, 0), (64, 64)]
    assert all(result.streams[tile.stream_index].is_dependent for tile in grid.tiles)

    additional = result.additional_images()
    assert additional.stream_indices == []
    assert [grid.title for grid in result.tile_grids if grid.id in additional.tile_grid_ids] == ["Alpha"]
    assert result.to_dict()["tile_grids"][0]["is_primary"] is True


def test_a_single_avif_reports_its_alpha_plane_as_an_additional_image():
    result = lossylab.probe(lossylab.Source.from_path(str(DATA_DIR / "testsrc_128x96_alpha.avif")))
    assert result.primary_video_stream().is_default is True
    assert result.additional_images().stream_indices == [1]
    assert result.streams[1].metadata["title"] == "Alpha"


def jpeg_of(name):
    return lossylab.probe(lossylab.Source.from_path(str(DATA_DIR / name))).primary_video_stream().jpeg


@pytest.mark.parametrize(
    ("fixture", "quality", "huffman", "process"),
    [
        ("testsrc_64x48_q75.jpg", 75, "standard", "baseline"),
        ("testsrc_64x48_q90_optimized_444.jpg", 90, "custom", "baseline"),
        ("testsrc_64x48_q85_progressive.jpg", 85, "custom", "progressive"),
        ("testsrc_64x48_q50_gray.jpg", 50, "standard", "baseline"),
    ],
)
def test_pillow_jpegs_report_their_libjpeg_quality(fixture, quality, huffman, process):
    info = jpeg_of(fixture)
    assert info.ijg_quality == quality
    assert info.ijg_quality_exact is True
    assert info.huffman_tables == huffman
    assert info.process == process
    assert info.has_end_of_image is True


def test_jpeg_tables_segments_and_comment_are_exposed():
    info = jpeg_of("testsrc_64x48_q90_optimized_444.jpg")
    assert len(info.quantization_tables[0].values) == 64
    assert info.segments[0].identifier == "JFIF"
    assert info.comment == "lossylab test fixture"
    assert info.to_dict()["ijg_quality"] == 90


def test_an_ffmpeg_written_jpeg_is_not_a_libjpeg_match():
    info = jpeg_of("testsrc_64x48.jpg")
    assert info.comment.startswith("Lavc")
    assert info.ijg_quality_exact is False


def test_to_dict_round_trips_through_json():
    source = lossylab.Source.from_path(str(DATA_DIR / "testsrc_64x48.png"))
    document = lossylab.probe(source).to_dict()
    assert json.loads(json.dumps(document)) == document
