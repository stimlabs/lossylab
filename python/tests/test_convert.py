import io
from pathlib import Path

import numpy
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


def _png_with_profile(pixels, profile_bytes):
    """PNG bytes of an RGB array, with the given ICC profile embedded if any."""
    from PIL import Image

    buffer = io.BytesIO()
    image = Image.fromarray(pixels, "RGB")
    image.save(buffer, format="PNG", **({"icc_profile": profile_bytes} if profile_bytes else {}))
    return buffer.getvalue()


def _decode_to_srgb24(data):
    options = lossylab.DecodeImageOptions()
    conversion = lossylab.ConvertOptions()
    conversion.pixel_format = lossylab.PixelFormat.from_name("rgb24")
    conversion.color = lossylab.ColorSpec.srgb()
    options.conversion = conversion
    return lossylab.decode_image(lossylab.Source.from_bytes(data), options)


def _random_rgb(height=24, width=40):
    return numpy.random.default_rng(7).integers(0, 256, (height, width, 3), dtype=numpy.uint8)


def test_an_adobe_rgb_profile_converts_like_lcms2_in_pillow():
    from PIL import Image, ImageCms

    pixels = _random_rgb()
    adobe_rgb = (DATA_DIR / "icc" / "adobe_rgb_colord.icc").read_bytes()
    decoded = _decode_to_srgb24(_png_with_profile(pixels, adobe_rgb))
    assert decoded.frame.icc_profile() is None
    assert _has_conversion(decoded.record.conversions, "icc_profile")

    reference = ImageCms.profileToProfile(
        Image.fromarray(pixels, "RGB"),
        ImageCms.ImageCmsProfile(io.BytesIO(adobe_rgb)),
        ImageCms.createProfile("sRGB"),
        renderingIntent=ImageCms.Intent.RELATIVE_COLORIMETRIC,
        flags=ImageCms.Flags.BLACKPOINTCOMPENSATION | ImageCms.Flags.NOOPTIMIZE,
    )
    assert numpy.array_equal(numpy.asarray(decoded.frame.plane(0)), numpy.asarray(reference))


def test_an_srgb_profile_and_no_profile_decode_identically():
    pixels = _random_rgb()
    srgb = (DATA_DIR / "icc" / "srgb_colord.icc").read_bytes()
    srgb_d50 = (DATA_DIR / "icc" / "srgb_d50_v2.icc").read_bytes()
    untagged = _decode_to_srgb24(_png_with_profile(pixels, None))
    assert numpy.array_equal(numpy.asarray(untagged.frame.plane(0)), pixels)
    for profile in (srgb, srgb_d50):
        tagged = _decode_to_srgb24(_png_with_profile(pixels, profile))
        assert not _has_conversion(tagged.record.conversions, "icc_profile")
        assert numpy.array_equal(numpy.asarray(tagged.frame.plane(0)), pixels)


def test_a_cmyk_profile_is_recorded_as_not_applied():
    from PIL import Image

    cmyk = numpy.random.default_rng(3).integers(0, 256, (16, 16, 4), dtype=numpy.uint8)
    buffer = io.BytesIO()
    Image.fromarray(cmyk, "CMYK").save(
        buffer, format="JPEG", quality=95, icc_profile=(DATA_DIR / "icc" / "cmyk_lut_v4.icc").read_bytes()
    )
    decoded = _decode_to_srgb24(buffer.getvalue())
    events = {event.property: event.to for event in decoded.record.conversions}
    assert events["icc_profile"] == "not applied: CMYK folded to RGB by the decoder"
    assert decoded.frame.icc_profile() is None
