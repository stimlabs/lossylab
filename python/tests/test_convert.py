from pathlib import Path

import pytest

import lossylab

DATA_DIR = Path(__file__).resolve().parents[2] / "tests" / "data"


def _has_conversion(conversions, property_name):
    return any(event.property == property_name for event in conversions)


def _decode(name):
    return lossylab.decode_image(lossylab.Source.from_path(str(DATA_DIR / name))).frame


def _bt709_yuv_keeping_gamut_of(frame):
    """BT.709 limited YUV with the frame's own primaries and transfer, which convert() cannot change."""
    color = lossylab.ColorSpec.bt709_limited()
    color.primaries = frame.color().primaries
    color.transfer = frame.color().transfer
    return color


def test_convert_convenience_overload_changes_format_and_color():
    frame = _decode("testsrc_64x48.png")
    target = _bt709_yuv_keeping_gamut_of(frame)
    result = lossylab.convert(frame, lossylab.PixelFormat.from_name("yuv420p"), target)

    assert result.frame.pixel_format().name() == "yuv420p"
    assert result.frame.color() == target
    assert result.frame.width() == frame.width()
    assert result.frame.height() == frame.height()

    record = result.record
    assert record.transform.is_identity()
    assert record.input == frame.describe()
    assert record.output == result.frame.describe()
    assert record.kind == lossylab.StageKind.Convert
    assert record.implementation == "swscale"
    assert _has_conversion(record.conversions, "pix_fmt")
    assert _has_conversion(record.conversions, "subsampling")


def test_convert_options_overload_matches_convenience_overload():
    frame = _decode("testsrc_64x48.png")
    options = lossylab.ConvertOptions()
    options.pixel_format = lossylab.PixelFormat.from_name("yuv420p")
    options.color = _bt709_yuv_keeping_gamut_of(frame)
    result = lossylab.convert(frame, options)

    assert result.frame.pixel_format().name() == "yuv420p"
    assert result.frame.color() == options.color


def test_strict_refuse_permits_the_rgb_to_yuv_matrix_change_it_entails():
    frame = _decode("testsrc_64x48.png")
    # Match every color field except matrix, so the only conversion this
    # entails is the RGB/YUV matrix switch itself, which Strict.Refuse always
    # exempts (the switch can't happen any other way).
    target_color = lossylab.ColorSpec()
    target_color.matrix = lossylab.ColorMatrix.Bt709
    target_color.range = frame.color().range
    target_color.primaries = frame.color().primaries
    target_color.transfer = frame.color().transfer
    target_color.chroma_location = lossylab.ChromaLocation.Left

    result = lossylab.convert(
        frame, lossylab.PixelFormat.from_name("yuv420p"), target_color, strict=lossylab.Strict.Refuse,
    )
    assert result.frame.pixel_format().name() == "yuv420p"


def test_strict_refuse_rejects_conversions_beyond_the_rgb_yuv_boundary():
    frame = _decode("testsrc_64x48.png")
    with pytest.raises(lossylab.ConversionRefused):
        lossylab.convert(
            frame, lossylab.PixelFormat.from_name("yuv420p"), _bt709_yuv_keeping_gamut_of(frame),
            strict=lossylab.Strict.Refuse,
        )


def test_a_gamut_or_tone_curve_change_is_not_performed():
    frame = _decode("testsrc_64x48.png")
    with pytest.raises(lossylab.NotImplemented):
        lossylab.convert(frame, lossylab.PixelFormat.from_name("yuv420p"), lossylab.ColorSpec.bt709_limited())


def test_reinterpret_does_not_touch_samples():
    frame = _decode("testsrc_64x48.png")
    reinterpreted = lossylab.reinterpret(frame, lossylab.ColorSpec.jpeg())
    assert reinterpreted.frame.color() == lossylab.ColorSpec.jpeg()
    assert reinterpreted.record.kind == lossylab.StageKind.Reinterpret

    original = frame.plane(0)
    relabeled = reinterpreted.frame.plane(0)
    assert (original == relabeled).all()


def test_chroma_roundtrip_applies_a_realistic_chroma_history():
    frame = _decode("testsrc_64x48.png")
    options = lossylab.ChromaRoundtripOptions()
    options.subsampling = lossylab.Subsampling.Yuv420
    result = lossylab.chroma_roundtrip(frame, options)

    assert result.frame.pixel_format() == frame.pixel_format()
    assert result.record.kind == lossylab.StageKind.ChromaRoundtrip
