from pathlib import Path

import numpy as np
from PIL import Image

import lossylab

DATA_DIR = Path(__file__).resolve().parents[2] / "tests" / "data"


def _source(name):
    return lossylab.Source.from_path(str(DATA_DIR / name))


def test_describe_icc_profile_recognizes_display_p3():
    info = lossylab.describe_icc_profile((DATA_DIR / "icc" / "display_p3_v4.icc").read_bytes())
    assert info.known_as == "Display P3"
    assert info.primaries == lossylab.ColorPrimaries.Smpte432
    assert info.transfer == lossylab.TransferCharacteristic.Srgb
    assert info.is_expressible_as_tags()
    assert abs(info.colorants.red.x - 0.680) < 0.002
    assert info.to_dict()["known_as"] == "Display P3"


def test_describe_icc_profile_reports_garbage_instead_of_raising():
    info = lossylab.describe_icc_profile(b"not a profile")
    assert info.problems


def test_probe_reports_orientation_and_profile():
    stream = lossylab.probe(_source("testsrc_64x48_p3_orientation6.jpg")).primary_video_stream()
    assert stream.orientation == 6
    assert stream.orientation_source == "exif"
    assert stream.orientation_availability == lossylab.Availability.Present
    assert stream.icc_profile.known_as == "Display P3"
    assert stream.icc_profile_availability == lossylab.Availability.Present
    assert stream.icc_matches_tagged_color is None


def test_probe_reports_a_grid_orientation():
    grid = lossylab.probe(_source("testsrc_128x96_grid_irot3.avif")).primary_tile_grid()
    assert grid.orientation == 6


def test_applying_the_orientation_matches_pillows_exif_transpose():
    options = lossylab.DecodeImageOptions()
    options.orientation = lossylab.OrientationHandling.Apply
    result = lossylab.decode_image(_source("testsrc_64x48_orientation6.png"), options)

    upright = np.asarray(Image.open(DATA_DIR / "testsrc_64x48_orientation6_upright.png").convert("RGB"))
    np.testing.assert_array_equal(np.asarray(result.frame.plane(0)), upright)
    assert result.record.transform == lossylab.CoordinateTransform.orientation(6, 64, 48)
    assert result.record.to_dict()["params"]["orientation_handling"] == "applied"


def test_reporting_leaves_the_pixels_as_stored():
    result = lossylab.decode_image(_source("testsrc_64x48_orientation6.png"))
    assert result.frame.width() == 64
    assert result.record.to_dict()["params"]["orientation"] == 6


def test_a_recognized_profile_stands_in_for_missing_tags():
    frame = lossylab.decode_image(_source("testsrc_64x48_p3.png")).frame
    assert frame.color().primaries == lossylab.ColorPrimaries.Smpte432
