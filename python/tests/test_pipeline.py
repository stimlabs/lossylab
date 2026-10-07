import gc
import json
from pathlib import Path

import numpy
import pytest

import lossylab

DATA_DIR = Path(__file__).resolve().parents[2] / "tests" / "data"
JPEG = DATA_DIR / "testsrc_64x48_q75.jpg"


def _to_srgb24():
    options = lossylab.ConvertOptions()
    options.pixel_format = lossylab.PixelFormat.from_name("rgb24")
    options.color = lossylab.ColorSpec.srgb()
    return options


def _equalization():
    """Decode to sRGB, crop on the JPEG grid, orient, achromatic, JPEG at qscale 2 decoded back."""
    decode = lossylab.DecodeImageOptions()
    decode.conversion = _to_srgb24()

    crop = lossylab.CropOptions()
    crop.x, crop.y, crop.width, crop.height = 16, 16, 32, 16

    orient = lossylab.OrientOptions()
    orient.orientation = 6

    jpeg_color = lossylab.ColorSpec.jpeg()
    jpeg_color.primaries = lossylab.ColorPrimaries.Bt709
    jpeg_color.transfer = lossylab.TransferCharacteristic.Srgb
    roundtrip = lossylab.RoundtripImageConfiguration()
    roundtrip.encode.codec = lossylab.ImageCodec.Mjpeg
    roundtrip.encode.pixel_format = lossylab.PixelFormat.from_name("yuvj420p")
    roundtrip.encode.color = jpeg_color
    roundtrip.encode.rate_control = lossylab.RateControl.quality(2)
    roundtrip.encode.strict = lossylab.Strict.AllowRecorded
    roundtrip.decode.conversion = _to_srgb24()

    spec = lossylab.PipelineSpec()
    spec.add(decode).add(crop).add(orient).add(lossylab.AchromaticOptions()).add(roundtrip, "jpeg q2")
    return spec


def test_the_equalization_chain_ends_in_a_contiguous_rgb_array():
    result = lossylab.Pipeline(_equalization()).run(lossylab.Source.from_path(str(JPEG)), seed=3)
    pixels = result.frame.to_numpy()
    assert pixels.shape == (32, 16, 3)
    assert pixels.dtype == numpy.uint8
    assert pixels.flags.c_contiguous
    assert numpy.array_equal(pixels[..., 0], pixels[..., 2])

    record = result.record
    assert [stage.kind for stage in record.stages()] == [
        lossylab.StageKind.DecodeImage,
        lossylab.StageKind.Crop,
        lossylab.StageKind.Orient,
        lossylab.StageKind.Achromatic,
        lossylab.StageKind.RoundtripImage,
    ]
    assert record.seed == 3
    assert record.output_sha256 == result.frame.samples_sha256()
    json.dumps(record.to_dict())

    del result
    gc.collect()
    assert pixels.sum() > 0


def test_the_array_is_a_copy_when_rows_are_padded():
    frame = lossylab.Frame.allocate(33, 5, lossylab.PixelFormat.from_name("rgb24"), lossylab.ColorSpec.srgb())
    frame.writable_plane(0)[...] = 7
    pixels = frame.to_numpy()
    assert pixels.shape == (5, 33, 3) and pixels.flags.c_contiguous
    assert numpy.all(pixels == 7)
    planar = lossylab.Frame.allocate(
        4, 4, lossylab.PixelFormat.from_name("yuv420p"), lossylab.ColorSpec.bt709_limited()
    )
    with pytest.raises(lossylab.ConfigError):
        planar.to_numpy()


def test_a_spec_round_trips_and_a_record_replays():
    spec = _equalization()
    assert lossylab.PipelineSpec.from_dict(spec.to_dict()) == spec
    assert lossylab.PipelineSpec.parse(json.dumps(spec.to_dict())).spec_id() == spec.spec_id()
    assert spec.stages()[4].label == "jpeg q2"

    source = lossylab.Source.from_path(str(JPEG))
    first = lossylab.Pipeline(spec).run(source)
    stored = lossylab.ProcessingRecord.from_dict(json.loads(json.dumps(first.record.to_dict())))
    replay = lossylab.PipelineSpec.from_record(stored)
    assert replay.source_sha256 == source.sha256()
    assert lossylab.Pipeline(replay).run(source).record.output_sha256 == first.record.output_sha256


def test_capture_run_returns_failures_as_data():
    spec = _equalization()
    spec.source_sha256 = lossylab.Source.from_path(str(DATA_DIR / "testsrc_64x48.png")).sha256()
    result = lossylab.capture_run(lossylab.Source.from_path(str(JPEG)), lossylab.Pipeline(spec))
    assert not result
    assert result.error().kind == lossylab.FileErrorKind.Config

    broken = lossylab.capture_run(lossylab.Source.from_bytes(b"not an image"), lossylab.Pipeline(_equalization()))
    assert not broken.ok()

    working = lossylab.capture_run(lossylab.Source.from_path(str(JPEG)), lossylab.Pipeline(_equalization()))
    assert working.ok()
    assert working.value().frame.to_numpy().shape == (32, 16, 3)


def test_a_pipeline_on_a_frame_skips_the_decode():
    frame = lossylab.decode_image(lossylab.Source.from_path(str(DATA_DIR / "testsrc_64x48.png"))).frame
    spec = lossylab.PipelineSpec()
    spec.add(lossylab.AchromaticOptions())
    result = lossylab.Pipeline(spec).run(frame)
    assert result.record.origin is None
    assert result.frame.to_numpy().shape == (48, 64, 3)


def test_a_lean_record_leaves_out_the_build_and_the_hash():
    source = lossylab.Source.from_path(str(JPEG))
    pipeline = lossylab.Pipeline(_equalization())
    full = pipeline.run(source, seed=5)
    lean = pipeline.run(source, seed=5, record_detail=lossylab.RecordDetail.Lean)

    assert numpy.array_equal(lean.frame.to_numpy(), full.frame.to_numpy())
    assert [stage.kind for stage in lean.record.stages()] == [stage.kind for stage in full.record.stages()]
    assert lean.record.seed == 5
    assert lean.record.origin is not None
    assert lean.record.build is None and lean.record.diagnostics is None
    assert lean.record.output_sha256 is None
    assert full.record.build is not None

    replay = lossylab.PipelineSpec.from_record(lean.record)
    assert pipeline.run(source).record.output_sha256 == lossylab.Pipeline(replay).run(source).record.output_sha256

    captured = lossylab.capture_run(source, pipeline, seed=5, record_detail=lossylab.RecordDetail.Lean)
    assert captured.ok()
    assert captured.value().record.output_sha256 is None


def test_an_array_runs_through_a_pipeline():
    pixels = numpy.random.default_rng(0).integers(0, 256, size=(24, 40, 3), dtype=numpy.uint8)
    spec = lossylab.PipelineSpec()
    crop = lossylab.CropOptions()
    crop.x, crop.y, crop.width, crop.height = 8, 4, 16, 8
    spec.add(crop)

    result = lossylab.Pipeline(spec).run(lossylab.Frame.from_numpy(pixels), record_detail=lossylab.RecordDetail.Lean)
    output = result.frame.to_numpy(writable=True)
    assert numpy.array_equal(output, pixels[4:12, 8:24])
    assert output.flags.writeable
    assert numpy.array_equal(numpy.from_dlpack(result.frame), pixels[4:12, 8:24])
