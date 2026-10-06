import io
from pathlib import Path

import numpy as np
import pytest

import lossylab

DATA_DIR = Path(__file__).resolve().parents[2] / "tests" / "data"


def bt601(color_range):
    color = lossylab.ColorSpec.srgb()
    color.matrix = lossylab.ColorMatrix.Bt470bg
    color.range = color_range
    color.chroma_location = lossylab.ChromaLocation.Center
    return color


def jpeg_ready_frame():
    rgb = lossylab.decode_image(lossylab.Source.from_path(str(DATA_DIR / "testsrc_64x48.png"))).frame
    return lossylab.convert(rgb, lossylab.PixelFormat.from_name("yuvj420p"), bt601(lossylab.ColorRange.Full)).frame


def mjpeg_options(qscale):
    options = lossylab.EncodeImageOptions()
    options.codec = lossylab.ImageCodec.Mjpeg
    options.pixel_format = lossylab.PixelFormat.from_name("yuvj420p")
    options.rate_control = lossylab.RateControl.quality(qscale)
    return options


def jpeg_options(quality, pixel_format):
    options = lossylab.EncodeImageOptions()
    options.codec = lossylab.ImageCodec.Jpeg
    options.pixel_format = lossylab.PixelFormat.from_name(pixel_format)
    options.rate_control = lossylab.RateControl.quality(quality)
    return options


def moving_clip(frame_count):
    frames = []
    for index in range(frame_count):
        frame = lossylab.Frame.allocate(
            64, 64, lossylab.PixelFormat.from_name("yuv420p"), lossylab.ColorSpec.bt709_limited()
        )
        rows, columns = np.indices((64, 64))
        frame.writable_plane(0)[...] = (16 + ((rows + columns + 2 * index) * 7) % 220).astype(np.uint8)
        frame.writable_plane(1)[...] = 128
        frame.writable_plane(2)[...] = 128
        frames.append(frame)
    return frames


def test_encode_image_returns_a_file_and_its_record():
    encoded = lossylab.encode_image(jpeg_ready_frame(), mjpeg_options(5))
    assert encoded.bytes[:2] == b"\xff\xd8"
    assert encoded.record.kind == lossylab.StageKind.EncodeImage
    assert encoded.record.frames[0].qp_mean == 5.0
    assert encoded.bits_per_pixel() == pytest.approx(len(encoded.bytes) * 8 / (64 * 48))

    decoded = lossylab.decode_image(lossylab.Source.from_bytes(encoded.bytes)).frame
    assert (decoded.width(), decoded.height()) == (64, 48)


def test_jpeg_encodes_at_an_ijg_quality():
    encoded = lossylab.encode_image(jpeg_ready_frame(), jpeg_options(85, "yuvj420p"))
    assert encoded.bytes[:2] == b"\xff\xd8"
    assert encoded.record.implementation == "libjpeg-turbo"
    assert encoded.record.evidence.resolved.options["quality"] == "85"

    jpeg = lossylab.probe(lossylab.Source.from_bytes(encoded.bytes)).streams[0].jpeg
    assert jpeg.ijg_quality == 85
    assert jpeg.ijg_quality_exact


@pytest.mark.parametrize("quality", [1, 10, 50, 75, 95, 100])
def test_jpeg_writes_the_bytes_pillow_writes_for_the_same_ycbcr(quality):
    image_module = pytest.importorskip("PIL.Image")
    rgb = lossylab.decode_image(lossylab.Source.from_path(str(DATA_DIR / "testsrc_64x48.png"))).frame
    frame = lossylab.convert(rgb, lossylab.PixelFormat.from_name("yuvj444p"), bt601(lossylab.ColorRange.Full)).frame
    encoded = lossylab.encode_image(frame, jpeg_options(quality, "yuvj444p"))

    # 4:4:4 YCbCr goes through Pillow's libjpeg-turbo without a conversion or resampling as well.
    ycbcr = np.stack([np.asarray(frame.plane(index)) for index in range(3)], axis=-1)
    pillow_bytes = io.BytesIO()
    image_module.frombytes("YCbCr", (frame.width(), frame.height()), ycbcr.tobytes()).save(
        pillow_bytes, format="JPEG", quality=quality, subsampling=0
    )
    assert encoded.bytes == pillow_bytes.getvalue()


def test_an_image_roundtrip_is_one_record():
    source = jpeg_ready_frame()
    result = lossylab.roundtrip(source, mjpeg_options(5))
    assert result.record.kind == lossylab.StageKind.RoundtripImage
    assert isinstance(result.record.evidence, lossylab.RoundtripImageEvidence)
    assert result.record.evidence.encode.extension == "jpg"
    assert isinstance(result.configuration, lossylab.RoundtripImageConfiguration)
    assert result.configuration.encode.codec == lossylab.ImageCodec.Mjpeg
    psnr = lossylab.compare(source, result.frame, [lossylab.Metric.Psnr]).evidence.frames[0]["psnr"]
    assert 25 < psnr < 60


def test_a_clip_encodes_and_roundtrips():
    clip = moving_clip(8)
    options = lossylab.EncodeVideoOptions()
    options.codec = lossylab.VideoCodec.H264
    options.pixel_format = lossylab.PixelFormat.from_name("yuv420p")
    options.rate_control = lossylab.RateControl.crf(23)
    options.gop.keyframe_interval = 4
    options.gop.scene_change_detection = False

    encoded = lossylab.encode_video(clip, options)
    assert [stats.index for stats in encoded.record.frames if stats.key_frame] == [0, 4]

    result = lossylab.roundtrip(clip, options)
    assert len(result.frames) == len(clip)
    assert lossylab.compare(clip, result.frames, [lossylab.Metric.Psnr]).evidence.pooled["psnr_mean"] > 30
    assert result.record.kind == lossylab.StageKind.RoundtripVideo
    assert result.configuration.decode.frame_indices == list(range(len(clip)))


def test_encode_to_target_reaches_a_bits_per_pixel_target():
    target = lossylab.EncodeTarget()
    target.kind = lossylab.EncodeTarget.Kind.BitsPerPixel
    target.value = 3.0
    target.tolerance = 0.15
    result = lossylab.encode_to_target(jpeg_ready_frame(), mjpeg_options(10), target)
    assert result.search.converged
    assert result.search.achieved == pytest.approx(3.0, abs=0.15)
    assert result.search.quality_parameter in [attempt.quality_parameter for attempt in result.search.attempts]
    assert result.configuration.rate_control.quality_parameter() == result.search.quality_parameter


def test_rate_control_round_trips_through_a_dict():
    control = lossylab.RateControl.constrained(1_000_000, 2_000_000, 500_000)
    assert lossylab.RateControl.from_dict(control.to_dict()).describe() == control.describe()
    assert control.mode == lossylab.RateControl.Mode.Constrained
    assert control.quality_parameter() is None
