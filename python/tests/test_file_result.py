from pathlib import Path

import pytest

import lossylab

DATA_DIR = Path(__file__).resolve().parents[2] / "tests" / "data"


def truncated_mp4():
    # The fixture's moov atom is at the end, so its first half cannot be demuxed.
    data = (DATA_DIR / "testsrc_64x48.mp4").read_bytes()
    return lossylab.Source.from_bytes(data[: len(data) // 2])


def damaged_mp4():
    # Flipped bits in the first frame's slice data: decodes, with logged damage.
    data = bytearray((DATA_DIR / "testsrc_64x48.mp4").read_bytes())
    for i in range(1200, 1500):
        data[i] ^= 0x55
    return lossylab.Source.from_bytes(bytes(data))


def test_a_failed_capture_keeps_what_ffmpeg_logged():
    result = lossylab.capture_probe(truncated_mp4())
    assert not result.ok()
    assert any("moov atom not found" in message.text for message in result.log())
    assert result.log()[0].level == lossylab.LogLevel.Error
    assert result.to_dict()["log"][0]["level"] == "error"


def test_a_damaged_video_decodes_with_its_errors_logged():
    result = lossylab.capture_video_frames(damaged_mp4(), lossylab.FrameSelector.all())
    assert result.ok()
    assert any("concealing" in message.text for message in result.log())


def test_a_clean_capture_has_an_empty_log():
    result = lossylab.capture_video_frames(
        lossylab.Source.from_path(str(DATA_DIR / "testsrc_64x48.mp4")), lossylab.FrameSelector.all()
    )
    assert result.log() == []


def test_threads_keep_their_logs_apart():
    from concurrent.futures import ThreadPoolExecutor

    def run(index):
        if index % 2 == 0:
            return lossylab.capture_probe(truncated_mp4()).log()
        return lossylab.capture_probe(lossylab.Source.from_path(str(DATA_DIR / "testsrc_64x48.png"))).log()

    with ThreadPoolExecutor(max_workers=4) as pool:
        logs = list(pool.map(run, range(16)))
    for index, log in enumerate(logs):
        if index % 2 == 0:
            assert any("moov atom not found" in message.text for message in log)
        else:
            assert log == []


def test_capture_probe_returns_the_value_on_success():
    result = lossylab.capture_probe(lossylab.Source.from_path(str(DATA_DIR / "testsrc_64x48.png")))
    assert result.ok()
    assert result
    assert result.value().primary_video_stream().width == 64


def test_capture_probe_turns_garbage_bytes_into_an_ffmpeg_error():
    source = lossylab.Source.from_bytes(b"\x5a" * 256)
    result = lossylab.capture_probe(source)
    assert not result.ok()
    assert not result
    error = result.error()
    assert error.kind == lossylab.FileErrorKind.FFmpeg
    assert error.operation == "probe"
    assert error.source == source.describe()
    assert error.details["averror"] < 0
    assert error.details["call"]


def test_capture_decode_image_returns_the_frame_on_success():
    result = lossylab.capture_decode_image(lossylab.Source.from_path(str(DATA_DIR / "testsrc_64x48.png")))
    assert result.ok()
    assert result.value().frame.width() == 64
    assert result.value().record.kind == lossylab.StageKind.Decode
    as_dict = result.to_dict()
    assert as_dict["value"]["probe"]["format"] == "png_pipe"
    assert as_dict["value"]["record"]["kind"] == "decode"


def test_capture_decode_image_turns_a_missing_file_into_an_error():
    source = lossylab.Source.from_path(str(DATA_DIR / "does_not_exist.png"))
    result = lossylab.capture_decode_image(source)
    assert not result.ok()
    assert result.error().operation == "decode_image"
    assert result.error().source == source.describe()


def test_value_on_a_failed_result_raises_config_error():
    result = lossylab.capture_probe(lossylab.Source.from_bytes(b"\x5a" * 256))
    with pytest.raises(lossylab.ConfigError):
        result.value()


def test_error_on_a_successful_result_raises_config_error():
    result = lossylab.capture_probe(lossylab.Source.from_path(str(DATA_DIR / "testsrc_64x48.png")))
    with pytest.raises(lossylab.ConfigError):
        result.error()


def test_file_error_dict_round_trip():
    error = lossylab.capture_probe(lossylab.Source.from_bytes(b"\x5a" * 256)).error()
    as_dict = error.to_dict()
    assert as_dict["kind"] == "ffmpeg"
    assert as_dict["operation"] == "probe"
    assert lossylab.FileError.from_dict(as_dict) == error


def test_probe_file_result_dict_marks_success_and_failure():
    succeeded = lossylab.capture_probe(lossylab.Source.from_path(str(DATA_DIR / "testsrc_64x48.png")))
    assert succeeded.to_dict()["ok"] is True
    assert succeeded.to_dict()["value"] == succeeded.value().to_dict()

    failed = lossylab.capture_probe(lossylab.Source.from_bytes(b"\x5a" * 256))
    assert failed.to_dict()["ok"] is False
    assert failed.to_dict()["error"] == failed.error().to_dict()
