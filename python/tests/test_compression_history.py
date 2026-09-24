import io

import numpy as np
import pytest
from PIL import Image, features

import lossylab


def texture(width=512, height=384, seed=3):
    """A never-compressed RGB image with the roughly 1/f spectrum of a photo, and independent color detail."""
    rng = np.random.default_rng(seed)
    frequencies = np.sqrt(np.fft.fftfreq(height)[:, None] ** 2 + np.fft.fftfreq(width)[None, :] ** 2)

    def pink_noise():
        spectrum = rng.normal(size=(height, width)) + 1j * rng.normal(size=(height, width))
        field = np.real(np.fft.ifft2(spectrum / np.maximum(frequencies, 1.0 / 64)))
        return field / field.std()

    luma = 128 + 45 * pink_noise()
    red = luma + 18 * pink_noise()
    blue = luma + 18 * pink_noise()
    green = luma - 0.5 * (red - luma) - 0.5 * (blue - luma)
    rgb = np.stack([red, green, blue], axis=-1) + rng.uniform(-1.5, 1.5, size=(height, width, 3))
    return Image.fromarray(np.clip(np.round(rgb), 1, 254).astype(np.uint8))


def through(image, image_format, **save_options):
    """The image after a save in `image_format` and a decode by PIL."""
    buffer = io.BytesIO()
    image.save(buffer, image_format, **save_options)
    return Image.open(io.BytesIO(buffer.getvalue()))


def as_png_bytes(image):
    buffer = io.BytesIO()
    image.save(buffer, "PNG")
    return buffer.getvalue()


def as_png(image):
    """The image saved as a PNG and decoded by lossylab, as an audit meets it."""
    return lossylab.decode_image(lossylab.Source.from_memory(as_png_bytes(image))).frame


def traces_of(history, evidence):
    return [trace for trace in history.traces if trace.evidence == evidence]


def without_recompression():
    options = lossylab.CompressionHistoryOptions()
    options.recompression_codecs = []
    return options


@pytest.mark.parametrize(
    ("quality", "subsampling", "expected"),
    [
        (75, 2, lossylab.Subsampling.Yuv420),
        (90, 0, lossylab.Subsampling.Yuv444),
        (50, 1, lossylab.Subsampling.Yuv422),
        (95, 2, lossylab.Subsampling.Yuv420),
    ],
)
def test_a_jpeg_saved_as_png_shows_its_quality_and_subsampling(quality, subsampling, expected):
    image = through(texture(), "JPEG", quality=quality, subsampling=subsampling)
    history = lossylab.compression_history(as_png(image), without_recompression())

    [jpeg] = traces_of(history, lossylab.TraceEvidence.JpegQuantization)
    assert jpeg.codec == lossylab.ImageCodec.Mjpeg
    assert jpeg.quality == quality
    assert jpeg.subsampling == expected
    assert history.jpeg.ijg_quality_lowest <= quality <= history.jpeg.ijg_quality_highest
    assert history.jpeg.luma.ijg_match == 1.0


def recovered_steps_agree(estimate, table):
    """Whether every step the estimate determined equals the file's own, both in natural order."""
    return all(step == expected for step, expected in zip(estimate.values, table) if step != 0)


@pytest.mark.parametrize(
    ("preset", "expected"),
    [
        ("web_low", lossylab.Subsampling.Yuv420),
        ("web_medium", lossylab.Subsampling.Yuv420),
        ("web_high", lossylab.Subsampling.Yuv444),
        ("web_very_high", lossylab.Subsampling.Yuv444),
        ("medium", lossylab.Subsampling.Yuv420),
    ],
)
def test_a_jpeg_with_photoshop_tables_is_found_with_a_low_libjpeg_match(preset, expected):
    image = through(texture(), "JPEG", quality=preset)
    history = lossylab.compression_history(as_png(image), without_recompression())

    assert history.jpeg.detected
    assert history.jpeg.chroma_subsampling == expected
    assert recovered_steps_agree(history.jpeg.luma, image.quantization[0])
    assert recovered_steps_agree(history.jpeg.chroma, image.quantization[1])
    assert history.jpeg.ijg_match <= 0.5
    assert history.jpeg.luma.ijg_match <= 0.5
    [jpeg] = traces_of(history, lossylab.TraceEvidence.JpegQuantization)
    assert jpeg.quality is None
    assert jpeg.subsampling == expected


def test_a_jpeg_with_a_flat_table_is_found_with_a_low_libjpeg_match():
    image = through(texture(), "JPEG", qtables=[[12] * 64, [12] * 64])
    history = lossylab.compression_history(as_png(image), without_recompression())

    assert history.jpeg.detected
    assert {step for step in history.jpeg.luma.values if step != 0} == {12}
    assert history.jpeg.ijg_match <= 0.5
    [jpeg] = traces_of(history, lossylab.TraceEvidence.JpegQuantization)
    assert jpeg.quality is None


def test_a_cropped_jpeg_is_found_on_its_shifted_grid():
    image = through(texture(), "JPEG", quality=75).crop((5, 3, 500, 380))
    history = lossylab.compression_history(as_png(image), without_recompression())
    assert history.jpeg.detected
    assert (history.jpeg.grid_x, history.jpeg.grid_y) == (3, 5)
    assert history.jpeg.ijg_quality == 75


def test_gray_and_alpha_pngs_are_analyzed():
    gray = through(texture().convert("L"), "JPEG", quality=60)
    history = lossylab.compression_history(as_png(gray), without_recompression())
    assert history.jpeg.ijg_quality == 60
    assert history.jpeg.chroma_subsampling == lossylab.Subsampling.Gray

    rgba = through(texture(), "JPEG", quality=80).convert("RGBA")
    history = lossylab.compression_history(as_png(rgba))
    assert history.jpeg.ijg_quality == 80
    assert history.record.conversions


def test_a_webp_saved_as_png_shows_its_quality():
    image = through(texture(), "WEBP", quality=80)
    history = lossylab.compression_history(as_png(image))

    assert not history.jpeg.detected
    [webp] = traces_of(history, lossylab.TraceEvidence.Recompression)
    assert webp.codec == lossylab.ImageCodec.WebP
    assert abs(webp.quality - 80) <= 3
    [chroma] = traces_of(history, lossylab.TraceEvidence.ChromaSubsampling)
    assert chroma.subsampling == lossylab.Subsampling.Yuv420
    assert history.chroma.upsampling == lossylab.ChromaUpsampling.Triangle


@pytest.mark.skipif(not features.check("jpg_2000"), reason="Pillow built without OpenJPEG")
@pytest.mark.parametrize("mode", ["RGB", "L"])
def test_an_openjpeg_file_saved_as_png_shows_a_jpeg_2000_trace(mode):
    options = lossylab.CompressionHistoryOptions()
    options.recompression_codecs = [lossylab.ImageCodec.Jpeg2000]
    image = through(texture().convert(mode), "JPEG2000", quality_mode="rates", quality_layers=[16], irreversible=True)

    # The ratio is in FFmpeg's nominal units, not OpenJPEG's true ones: this
    # file's 16 reads about 14 for gray and 42 for RGB.
    [trace] = traces_of(lossylab.compression_history(as_png(image), options), lossylab.TraceEvidence.Recompression)
    assert trace.codec == lossylab.ImageCodec.Jpeg2000
    assert trace.confidence >= 0.7

    pristine = lossylab.compression_history(as_png(texture().convert(mode)), options)
    assert traces_of(pristine, lossylab.TraceEvidence.Recompression) == []


def test_a_never_compressed_png_shows_no_trace():
    history = lossylab.compression_history(as_png(texture()))
    assert history.traces == []
    assert not history.jpeg.detected
    assert history.chroma.subsampling == lossylab.Subsampling.Yuv444


def test_a_resize_after_compression_erases_the_jpeg_trace():
    image = through(texture(), "JPEG", quality=75).resize((460, 345), Image.LANCZOS)
    history = lossylab.compression_history(as_png(image), without_recompression())
    assert not history.jpeg.detected


def test_capture_compression_history_serializes():
    source = lossylab.Source.from_memory(b"")
    result = lossylab.capture_compression_history(source, as_png(through(texture(), "JPEG", quality=75)))
    assert result
    document = result.to_dict()["value"]
    assert document["record"]["kind"] == "compression_history"
    assert any(trace["evidence"] == "jpeg_quantization" for trace in document["traces"])
    assert result.value().to_dict()["jpeg"]["ijg_quality"] == 75


def jpeg_bytes(image, **save_options):
    buffer = io.BytesIO()
    image.save(buffer, "JPEG", **save_options)
    return buffer.getvalue()


@pytest.mark.parametrize("quality", [75, 95, 100])
@pytest.mark.parametrize(
    ("subsampling", "expected"),
    [(0, lossylab.Subsampling.Yuv444), (1, lossylab.Subsampling.Yuv422), (2, lossylab.Subsampling.Yuv420)],
)
def test_a_jpeg_file_is_read_from_its_header(quality, subsampling, expected):
    image = lossylab.decode_image(
        lossylab.Source.from_memory(jpeg_bytes(texture(), quality=quality, subsampling=subsampling))
    )
    history = lossylab.compression_history(image, without_recompression())
    params = history.record.to_dict()["params"]
    assert params["jpeg_tables"] == "header"
    assert params["jpeg_header_unused"] is None
    assert history.jpeg.detected
    assert history.jpeg.ijg_quality == quality and history.jpeg.ijg_match == 1.0
    assert history.jpeg.chroma_subsampling == expected
    assert history.jpeg.grid_score is None and history.jpeg.luma.lattice_score is None
    [trace] = traces_of(history, lossylab.TraceEvidence.JpegHeader)
    assert trace.quality == quality and trace.confidence == 1.0
    assert traces_of(history, lossylab.TraceEvidence.JpegQuantization) == []


def test_a_quality_100_jpeg_file_is_found_only_from_its_header():
    data = jpeg_bytes(texture(), quality=100)
    image = lossylab.decode_image(lossylab.Source.from_memory(data))
    assert list(lossylab.compression_history(image, without_recompression()).jpeg.luma.values) == [1] * 64
    assert not lossylab.compression_history(image.frame, without_recompression()).jpeg.detected


def test_the_header_is_not_used_for_a_frame_it_does_not_describe():
    exif = Image.Exif()
    exif[0x0112] = 6
    rotated = lossylab.Source.from_memory(jpeg_bytes(texture(), quality=75, exif=exif))
    options = lossylab.DecodeImageOptions()
    options.orientation = lossylab.OrientationHandling.Apply
    cmyk = lossylab.Source.from_memory(jpeg_bytes(Image.new("CMYK", (64, 64), (10, 20, 30, 40)), quality=80))
    png = lossylab.Source.from_memory(as_png_bytes(texture()))
    for image, reason in [
        (lossylab.decode_image(rotated, options), "its orientation was applied"),
        (lossylab.decode_image(cmyk), "a JPEG of 4 components"),
        (lossylab.decode_image(png), "not a JPEG file"),
    ]:
        params = lossylab.compression_history(image, without_recompression()).record.to_dict()["params"]
        assert params["jpeg_tables"] == "pixels"
        assert params["jpeg_header_unused"] == reason


def test_the_pixel_check_agrees_with_the_header():
    options = without_recompression()
    options.jpeg_pixel_check = True
    image = lossylab.decode_image(lossylab.Source.from_memory(jpeg_bytes(texture(), quality=75)))
    history = lossylab.compression_history(image, options)
    assert history.jpeg_pixel_check.detected
    check = history.record.to_dict()["params"]["jpeg_pixel_check"]
    assert check["ijg_quality_equal"]
    assert check["luma"]["matching"] == check["luma"]["determined"]
    assert lossylab.compression_history(image, without_recompression()).jpeg_pixel_check is None


def test_capture_compression_history_takes_a_decoded_image():
    source = lossylab.Source.from_memory(jpeg_bytes(texture(), quality=75))
    result = lossylab.capture_compression_history(source, lossylab.decode_image(source))
    assert result
    document = result.to_dict()["value"]
    assert [trace["evidence"] for trace in document["traces"]] == ["jpeg_header"]
    assert document["jpeg"]["grid_score"] is None
