import io

import numpy as np
import pytest
from PIL import Image

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


def as_png(image):
    """The image saved as a PNG and decoded by lossylab, as an audit meets it."""
    buffer = io.BytesIO()
    image.save(buffer, "PNG")
    return lossylab.decode_image(lossylab.Source.from_memory(buffer.getvalue())).frame


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
