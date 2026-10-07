import numpy
import pytest

import lossylab


def _pixels(height=6, width=10):
    return numpy.arange(height * width * 3, dtype=numpy.uint8).reshape(height, width, 3)


def test_an_array_becomes_an_srgb_rgb24_frame():
    pixels = _pixels()
    frame = lossylab.Frame.from_numpy(pixels)
    assert (frame.width(), frame.height()) == (10, 6)
    assert frame.pixel_format() == lossylab.PixelFormat.from_name("rgb24")
    assert frame.color() == lossylab.ColorSpec.srgb()
    assert frame.icc_profile() is None
    assert frame.orientation() is None
    assert numpy.array_equal(frame.to_numpy(), pixels)

    pixels[...] = 0
    assert frame.to_numpy().sum() > 0


@pytest.mark.parametrize(
    "view",
    [
        lambda pixels: pixels[:, ::-1],
        lambda pixels: pixels[..., ::-1],
        lambda pixels: pixels[1:5, 2:9],
        lambda pixels: pixels[::2, ::3],
        lambda pixels: numpy.asfortranarray(pixels),
    ],
)
def test_any_strides_are_copied(view):
    pixels = view(_pixels())
    assert numpy.array_equal(lossylab.Frame.from_numpy(pixels).to_numpy(), pixels)


def test_a_read_only_array_is_accepted():
    pixels = _pixels()
    pixels.flags.writeable = False
    assert numpy.array_equal(lossylab.Frame.from_numpy(pixels).to_numpy(), pixels)


@pytest.mark.parametrize(
    ("array", "message"),
    [
        (numpy.zeros((6, 10, 3), dtype=numpy.uint16), "uint16"),
        (numpy.zeros((6, 10, 3), dtype=numpy.float32), "float32"),
        (numpy.zeros((6, 10), dtype=numpy.uint8), r"\(6, 10\)"),
        (numpy.zeros((6, 10, 4), dtype=numpy.uint8), r"\(6, 10, 4\)"),
    ],
)
def test_another_dtype_or_shape_is_refused(array, message):
    with pytest.raises(lossylab.ConfigError, match=message):
        lossylab.Frame.from_numpy(array)


def test_a_writable_array_is_a_view_of_unpadded_rows():
    frame = lossylab.Frame.from_numpy(numpy.zeros((4, 64, 3), dtype=numpy.uint8))
    assert not frame.to_numpy().flags.writeable

    view = frame.to_numpy(writable=True)
    assert view.flags.writeable
    assert numpy.shares_memory(view, frame.to_numpy())


def test_a_padded_frame_gives_a_writable_copy():
    frame = lossylab.Frame.allocate(33, 5, lossylab.PixelFormat.from_name("rgb24"), lossylab.ColorSpec.srgb())
    frame.writable_plane(0)[...] = 7
    copy = frame.to_numpy(writable=True)
    assert copy.flags.writeable and copy.flags.c_contiguous
    assert numpy.all(copy == 7)


def test_dlpack_exports_the_samples_with_their_row_stride():
    padded = lossylab.Frame.allocate(33, 5, lossylab.PixelFormat.from_name("rgb24"), lossylab.ColorSpec.srgb())
    padded.writable_plane(0)[...] = numpy.arange(33 * 3, dtype=numpy.uint8).reshape(33, 3)
    exported = numpy.from_dlpack(padded)
    assert exported.strides[0] > 33 * 3
    assert numpy.array_equal(exported, padded.to_numpy())

    frame = lossylab.Frame.from_numpy(_pixels())
    assert frame.__dlpack_device__() == (1, 0)
    assert numpy.array_equal(numpy.from_dlpack(frame), _pixels())


def test_dlpack_refuses_a_planar_frame():
    planar = lossylab.Frame.allocate(
        4, 4, lossylab.PixelFormat.from_name("yuv420p"), lossylab.ColorSpec.bt709_limited()
    )
    with pytest.raises(lossylab.ConfigError):
        numpy.from_dlpack(planar)
