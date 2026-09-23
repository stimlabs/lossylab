from pathlib import Path

import numpy as np
import pytest
from PIL import Image

import lossylab

DATA_DIR = Path(__file__).resolve().parents[2] / "tests" / "data"


def test_decode_png_matches_pil_reference():
    path = DATA_DIR / "testsrc_64x48.png"
    result = lossylab.decode_image(lossylab.Source.from_path(str(path)))
    frame = result.frame
    assert frame.width() == 64
    assert frame.height() == 48

    reference = np.asarray(Image.open(path).convert("RGB"))
    plane = frame.plane(0)
    assert plane.shape == reference.shape
    assert plane.dtype == np.uint8
    np.testing.assert_array_equal(np.asarray(plane), reference)


def test_plane_is_read_only():
    result = lossylab.decode_image(lossylab.Source.from_path(str(DATA_DIR / "testsrc_64x48.png")))
    plane = result.frame.plane(0)
    with pytest.raises(ValueError):
        plane[0, 0] = 0


def test_writable_plane_can_be_mutated_without_affecting_a_clone():
    result = lossylab.decode_image(lossylab.Source.from_path(str(DATA_DIR / "testsrc_64x48.png")))
    clone = result.frame.clone()

    writable = result.frame.writable_plane(0)
    original_value = int(clone.plane(0)[0, 0, 0])
    writable[0, 0, 0] = (original_value + 1) % 256

    assert int(result.frame.plane(0)[0, 0, 0]) == (original_value + 1) % 256
    assert int(clone.plane(0)[0, 0, 0]) == original_value


def test_plane_array_keeps_frame_alive():
    result = lossylab.decode_image(lossylab.Source.from_path(str(DATA_DIR / "testsrc_64x48.png")))
    plane = result.frame.plane(0)
    del result  # the ndarray's owner capsule must keep the Frame's buffer alive
    assert plane[0, 0, 0] >= 0


def software_av1_decoder_available():
    capabilities = lossylab.capabilities()
    return capabilities.has_decoder("libdav1d") or capabilities.has_decoder("libaom-av1")


@pytest.mark.skipif(not software_av1_decoder_available(), reason="no software AV1 decoder in this FFmpeg build")
def test_a_grid_avif_decodes_to_the_whole_image():
    result = lossylab.decode_image(lossylab.Source.from_path(str(DATA_DIR / "testsrc_128x96_grid_420.avif")))
    assert (result.frame.width(), result.frame.height()) == (128, 96)
    assert result.frame.plane(0).shape == (96, 128)
    assert result.record.params["tile_grid"]["tiles"] == 4


def test_decode_image_record_describes_the_decode():
    result = lossylab.decode_image(lossylab.Source.from_path(str(DATA_DIR / "testsrc_64x48.jpg")))
    assert result.record.kind == lossylab.StageKind.Decode
    assert result.record.transform.is_identity()
    assert result.record.output == result.frame.describe()
