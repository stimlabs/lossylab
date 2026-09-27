import gc
from pathlib import Path

import numpy as np
import pytest

import lossylab

DATA_DIR = Path(__file__).resolve().parents[2] / "tests" / "data"

# Five H.264 frames at 5 fps; picture types in presentation order are I B P I P.
VIDEO_PATH = DATA_DIR / "testsrc_64x48.mp4"


def fixture_reader(options=None):
    source = lossylab.Source.from_path(str(VIDEO_PATH))
    if options is None:
        return lossylab.VideoReader(source)
    return lossylab.VideoReader(source, options)


def test_frames_reads_every_frame():
    frames = fixture_reader().frames(lossylab.FrameSelector.all())
    assert [video_frame.index for video_frame in frames] == [0, 1, 2, 3, 4]
    assert [video_frame.stats.picture_type for video_frame in frames] == [
        lossylab.PictureType.I,
        lossylab.PictureType.B,
        lossylab.PictureType.P,
        lossylab.PictureType.I,
        lossylab.PictureType.P,
    ]
    assert frames[0].frame.plane(0).shape == (48, 64)


def test_stream_describes_the_video_stream():
    stream = fixture_reader().stream()
    assert stream.codec_name == "h264"
    assert stream.width == 64


def test_selectors_compose():
    select = lossylab.FrameSelector.picture_types([lossylab.PictureType.I]).and_also(
        lossylab.FrameSelector.timestamps([0.1])
    )
    frames = fixture_reader().frames(select)
    assert [video_frame.index for video_frame in frames] == [3]
    assert select.to_dict()["select"] == "and"


def test_a_python_predicate_selects_frames():
    frames = fixture_reader().frames(lossylab.FrameSelector.where(lambda candidate: candidate.index % 2 == 1))
    assert [video_frame.index for video_frame in frames] == [1, 3]


def test_an_exception_in_a_predicate_propagates():
    def predicate(candidate):
        raise RuntimeError("predicate failed")

    with pytest.raises(RuntimeError, match="predicate failed"):
        fixture_reader().frames(lossylab.FrameSelector.where(predicate))


def test_for_each_stops_when_the_callback_returns_false():
    reader = fixture_reader()
    seen = []

    def callback(video_frame):
        seen.append(video_frame.index)
        return len(seen) < 2

    reader.for_each(lossylab.FrameSelector.all(), callback)
    assert seen == [0, 1]
    assert len(reader.record().frames) == 2


def test_record_describes_the_last_read():
    reader = fixture_reader()
    reader.frames(lossylab.FrameSelector.stride(2))
    record = reader.record()
    assert record.kind == lossylab.StageKind.DecodeVideo
    assert record.implementation == "h264"
    assert isinstance(record.evidence, lossylab.DecodeVideoEvidence)
    assert record.evidence.source_sha256.startswith("sha256:")
    assert reader.configuration().frame_indices == [0, 2, 4]


def test_a_predicate_selection_replays_from_the_recorded_indices():
    reader = fixture_reader()
    picked = [frame.index for frame in reader.frames(lossylab.FrameSelector.where(lambda frame: frame.index > 2))]
    recorded = reader.configuration().frame_indices
    assert recorded == picked == [3, 4]
    replayed = reader.frames(lossylab.FrameSelector.indices(recorded))
    assert [frame.index for frame in replayed] == picked


def test_qp_maps_and_motion_vectors_are_exported_on_request():
    options = lossylab.VideoReaderOptions()
    options.export_qp_maps = True
    options.export_motion_vectors = True
    frames = fixture_reader(options).frames(lossylab.FrameSelector.all())

    assert all(video_frame.qp_map_availability == lossylab.Availability.Present for video_frame in frames)
    assert frames[0].qp_map.width == 4
    assert frames[0].stats.qp_mean is not None

    predicted = frames[1]
    assert predicted.motion_vector_availability == lossylab.Availability.Present
    assert predicted.motion_vectors[0].block_width > 0


def test_a_requested_conversion_is_applied():
    options = lossylab.VideoReaderOptions()
    options.pixel_format = lossylab.PixelFormat.from_name("rgb24")
    # RGB keeping the clip's BT.709 transfer, which convert() cannot change.
    color = lossylab.ColorSpec.srgb()
    color.transfer = lossylab.TransferCharacteristic.Bt709
    options.color = color
    frames = fixture_reader(options).frames(lossylab.FrameSelector.indices([0]))
    assert frames[0].frame.plane(0).shape == (48, 64, 3)


def test_the_reader_keeps_a_borrowed_buffer_alive():
    data = VIDEO_PATH.read_bytes()
    reader = lossylab.VideoReader(lossylab.Source.from_memory(data))
    del data
    gc.collect()
    assert len(reader.frames(lossylab.FrameSelector.all())) == 5


def test_repeated_reads_return_identical_pixels():
    reader = fixture_reader()
    first = reader.frames(lossylab.FrameSelector.indices([2]))[0]
    second = reader.frames(lossylab.FrameSelector.indices([2]))[0]
    np.testing.assert_array_equal(np.asarray(first.frame.plane(0)), np.asarray(second.frame.plane(0)))


def test_capture_video_frames_returns_frames_and_record():
    result = lossylab.capture_video_frames(
        lossylab.Source.from_path(str(VIDEO_PATH)), lossylab.FrameSelector.evenly_spaced(2)
    )
    assert result.ok()
    assert [video_frame.index for video_frame in result.value().frames] == [1, 3]
    assert result.value().configuration.frame_indices == [1, 3]


def test_capture_video_frames_turns_a_bad_file_into_an_error():
    source = lossylab.Source.from_bytes(b"\x5a" * 256)
    result = lossylab.capture_video_frames(source, lossylab.FrameSelector.all())
    assert not result.ok()
    assert result.error().operation == "VideoReader.frames"
    assert result.error().kind == lossylab.FileErrorKind.FFmpeg
