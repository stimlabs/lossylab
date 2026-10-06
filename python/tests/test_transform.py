from pathlib import Path

import numpy
import pytest

import lossylab

DATA_DIR = Path(__file__).resolve().parents[2] / "tests" / "data"


def _decode(name):
    return lossylab.decode_image(lossylab.Source.from_path(str(DATA_DIR / name)))


def _crop_options(x, y, width, height):
    options = lossylab.CropOptions()
    options.x, options.y, options.width, options.height = x, y, width, height
    return options


def test_a_crop_records_where_the_prior_grid_falls():
    jpeg = _decode("testsrc_64x48_q75.jpg")
    grid = jpeg.record.block_grid
    assert grid.kind == lossylab.BlockGridKind.JpegMcu
    rgb = lossylab.convert(jpeg.frame, lossylab.PixelFormat.from_name("rgb24"), lossylab.ColorSpec.srgb()).frame

    aligned = lossylab.crop(rgb, _crop_options(16, 16, 32, 16), grid)
    assert aligned.record.kind == lossylab.StageKind.Crop
    assert (aligned.record.evidence.block_grid.phase_x, aligned.record.evidence.block_grid.phase_y) == (0, 0)
    assert aligned.record.evidence.whole_blocks

    misaligned = lossylab.crop(rgb, _crop_options(5, 3, 32, 20), grid)
    assert misaligned.record.evidence.block_grid.phase_x != 0
    assert not misaligned.record.evidence.whole_blocks
    assert numpy.array_equal(numpy.asarray(misaligned.frame.plane(0)), numpy.asarray(rgb.plane(0))[3:23, 5:37])

    with pytest.raises(lossylab.ConfigError):
        lossylab.crop(rgb, _crop_options(40, 0, 32, 16))


def test_orient_turns_a_stored_image_upright():
    source = _decode("testsrc_64x48_orientation6.png").frame
    options = lossylab.OrientOptions()
    options.orientation = 6
    upright = lossylab.orient(source, options)
    assert upright.record.evidence.frame_orientation == 6
    assert upright.frame.orientation() is None
    expected = _decode("testsrc_64x48_orientation6_upright.png").frame
    assert numpy.array_equal(numpy.asarray(upright.frame.plane(0)), numpy.asarray(expected.plane(0)))


def test_achromatic_gives_equal_channels_and_reports_the_spread():
    gray = lossylab.achromatic(_decode("testsrc_64x48.png").frame)
    pixels = numpy.asarray(gray.frame.plane(0))
    assert numpy.array_equal(pixels[..., 0], pixels[..., 1])
    assert numpy.array_equal(pixels[..., 1], pixels[..., 2])
    evidence = gray.record.evidence
    assert evidence.channel_spread_max > 0
    assert evidence.channel_spread_std > 0.0
    assert isinstance(gray.configuration, lossylab.AchromaticOptions)
