"""Generates the ICC-profile and orientation fixtures under tests/data.

The synthesized profiles are written from first principles, so the values a test expects (primaries, transfer,
description) are the ones this script put there. The colord profiles are copied from the system (CC0-1.0).

Run from the repository root with the project's virtual environment:

    .venv/bin/python tests/data/generate_icc_orientation_fixtures.py

Needs Pillow, avifenc and the FFmpeg in /opt/ffmpeg.
"""

import hashlib
import logging
import shutil
import struct
import subprocess
import tempfile
from pathlib import Path

from PIL import Image, ImageOps

logger = logging.getLogger(__name__)

DATA_DIR = Path(__file__).resolve().parent
ICC_DIR = DATA_DIR / "icc"
COLORD_DIR = Path("/usr/share/color/icc/colord")
FFMPEG = "/opt/ffmpeg/bin/ffmpeg"

D50 = (0.9642, 1.0, 0.8249)
D65_XY = (0.3127, 0.3290)
DISPLAY_P3_XY = ((0.680, 0.320), (0.265, 0.690), (0.150, 0.060))
SRGB_XY = ((0.640, 0.330), (0.300, 0.600), (0.150, 0.060))

BRADFORD = ((0.8951, 0.2664, -0.1614), (-0.7502, 1.7135, 0.0367), (0.0389, -0.0685, 1.0296))

EXIF_ORIENTATION_TAG = 0x0112


def multiply(left, right):
    return tuple(tuple(sum(left[row][k] * right[k][column] for k in range(3)) for column in range(3)) for row in range(3))


def apply(matrix, vector):
    return tuple(sum(matrix[row][k] * vector[k] for k in range(3)) for row in range(3))


def inverse(matrix):
    (a, b, c), (d, e, f), (g, h, i) = matrix
    determinant = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g)
    return (
        ((e * i - f * h) / determinant, (c * h - b * i) / determinant, (b * f - c * e) / determinant),
        ((f * g - d * i) / determinant, (a * i - c * g) / determinant, (c * d - a * f) / determinant),
        ((d * h - e * g) / determinant, (b * g - a * h) / determinant, (a * e - b * d) / determinant),
    )


def xy_to_xyz(chromaticity):
    x, y = chromaticity
    return (x / y, 1.0, (1.0 - x - y) / y)


def bradford_adaptation(source_white, target_white):
    source_cone = apply(BRADFORD, source_white)
    target_cone = apply(BRADFORD, target_white)
    scale = tuple(tuple((target_cone[row] / source_cone[row]) if row == column else 0.0 for column in range(3))
                  for row in range(3))
    return multiply(inverse(BRADFORD), multiply(scale, BRADFORD))


def rgb_to_xyz_matrix(primaries, white_xy):
    columns = [xy_to_xyz(primary) for primary in primaries]
    primary_matrix = tuple(tuple(columns[column][row] for column in range(3)) for row in range(3))
    white = xy_to_xyz(white_xy)
    scale = apply(inverse(primary_matrix), white)
    return tuple(tuple(primary_matrix[row][column] * scale[column] for column in range(3)) for row in range(3))


def s15_fixed16(value):
    return struct.pack(">i", round(value * 65536))


def tag_xyz(xyz):
    return b"XYZ " + bytes(4) + b"".join(s15_fixed16(value) for value in xyz)


def tag_mluc(text):
    encoded = text.encode("utf-16-be")
    return b"mluc" + bytes(4) + struct.pack(">II", 1, 12) + b"enUS" + struct.pack(">II", len(encoded), 28) + encoded


def tag_v2_desc(text):
    ascii_text = text.encode("ascii") + b"\0"
    return b"desc" + bytes(4) + struct.pack(">I", len(ascii_text)) + ascii_text + bytes(4 + 4 + 2 + 1 + 67)


def tag_srgb_parametric():
    # Function type 3: Y = (aX + b)^g for X >= d, else cX.
    parameters = (2.4, 1 / 1.055, 0.055 / 1.055, 1 / 12.92, 0.04045)
    return b"para" + bytes(4) + struct.pack(">HH", 3, 0) + b"".join(s15_fixed16(value) for value in parameters)


def tag_sampled_curve(function, count):
    entries = [round(function(i / (count - 1)) * 65535) for i in range(count)]
    return b"curv" + bytes(4) + struct.pack(">I", count) + struct.pack(f">{count}H", *entries)


def tag_gamma_curve(gamma):
    return b"curv" + bytes(4) + struct.pack(">IH", 1, round(gamma * 256))


def tag_sf32(matrix):
    return b"sf32" + bytes(4) + b"".join(s15_fixed16(value) for row in matrix for value in row)


def srgb_decode(value):
    return value / 12.92 if value <= 0.04045 else ((value + 0.055) / 1.055) ** 2.4


def build_profile(version, device_class, color_space, connection_space, tags):
    """Lays out a profile: header, tag table, then each tag's data 4-byte aligned. Tags sharing one bytes object
    share one data block, as real profiles do for their three TRCs."""
    table_size = 4 + 12 * len(tags)
    offset = 128 + table_size
    entries = []
    blocks = []
    placed = {}
    for signature, data in tags:
        if id(data) not in placed:
            placed[id(data)] = offset
            padded = data + bytes(-len(data) % 4)
            blocks.append(padded)
            offset += len(padded)
        entries.append(signature.encode("ascii") + struct.pack(">II", placed[id(data)], len(data)))
    size = offset

    header = bytearray(128)
    struct.pack_into(">I", header, 0, size)
    header[8:12] = version
    header[12:16] = device_class.encode("ascii")
    header[16:20] = color_space.encode("ascii")
    header[20:24] = connection_space.encode("ascii")
    struct.pack_into(">6H", header, 24, 2026, 9, 23, 0, 0, 0)
    header[36:40] = b"acsp"
    header[68:80] = b"".join(s15_fixed16(value) for value in D50)
    header[80:84] = b"llab"

    profile = bytearray(header) + struct.pack(">I", len(tags)) + b"".join(entries) + b"".join(blocks)
    if version[0] >= 4:
        # The profile ID is the MD5 of the profile with the flags, rendering intent and ID fields zeroed.
        profile[84:100] = hashlib.md5(bytes(profile)).digest()
    return bytes(profile)


def display_p3_profile(description, trc_tags):
    to_xyz_d65 = rgb_to_xyz_matrix(DISPLAY_P3_XY, D65_XY)
    adaptation = bradford_adaptation(xy_to_xyz(D65_XY), D50)
    colorants = multiply(adaptation, to_xyz_d65)
    return build_profile(bytes([4, 0x30, 0, 0]), "mntr", "RGB ", "XYZ ", [
        ("desc", tag_mluc(description)),
        ("cprt", tag_mluc("No copyright, use freely")),
        ("wtpt", tag_xyz(D50)),
        ("rXYZ", tag_xyz(tuple(row[0] for row in colorants))),
        ("gXYZ", tag_xyz(tuple(row[1] for row in colorants))),
        ("bXYZ", tag_xyz(tuple(row[2] for row in colorants))),
        *trc_tags,
        ("chad", tag_sf32(adaptation)),
    ])


def d50_v2_profile(description, primaries):
    """A version 2 profile whose colorants are adapted to D50 but which carries neither a `chad` tag nor a media
    white point other than D50, like the Google 2016 sRGB profile."""
    colorants = multiply(bradford_adaptation(xy_to_xyz(D65_XY), D50), rgb_to_xyz_matrix(primaries, D65_XY))
    curve = tag_sampled_curve(srgb_decode, 1024)
    return build_profile(bytes([2, 0x10, 0, 0]), "mntr", "RGB ", "XYZ ", [
        ("desc", tag_v2_desc(description)),
        ("wtpt", tag_xyz(D50)),
        ("rXYZ", tag_xyz(tuple(row[0] for row in colorants))),
        ("gXYZ", tag_xyz(tuple(row[1] for row in colorants))),
        ("bXYZ", tag_xyz(tuple(row[2] for row in colorants))),
        ("rTRC", curve),
        ("gTRC", curve),
        ("bTRC", curve),
    ])


def write_d50_profiles():
    (ICC_DIR / "srgb_d50_v2.icc").write_bytes(d50_v2_profile("sRGB", SRGB_XY))
    (ICC_DIR / "display_p3_d50_v2.icc").write_bytes(
        d50_v2_profile("sRGB Transfer with Display P3 Gamut", DISPLAY_P3_XY))
    (ICC_DIR / "unknown_primaries_d50_v2.icc").write_bytes(
        d50_v2_profile("Calibrated display", ((0.600, 0.350), (0.280, 0.620), (0.160, 0.070))))


def write_profiles():
    ICC_DIR.mkdir(exist_ok=True)

    parametric = tag_srgb_parametric()
    (ICC_DIR / "display_p3_v4.icc").write_bytes(
        display_p3_profile("Display P3", [("rTRC", parametric), ("gTRC", parametric), ("bTRC", parametric)]))

    # Separate 32768-entry curves make the profile about 196 KB, so a JPEG must split it across four APP2 segments.
    sampled = [(name, tag_sampled_curve(srgb_decode, 32768)) for name in ("rTRC", "gTRC", "bTRC")]
    (ICC_DIR / "display_p3_sampled_v4.icc").write_bytes(display_p3_profile("Display P3 sampled", sampled))

    # A LUT-based output profile; only the A2B0 tag's presence matters to the parser.
    (ICC_DIR / "cmyk_lut_v4.icc").write_bytes(build_profile(bytes([4, 0x30, 0, 0]), "prtr", "CMYK", "Lab ", [
        ("desc", tag_mluc("Synthetic CMYK")),
        ("wtpt", tag_xyz(D50)),
        ("A2B0", b"mft2" + bytes(48)),
    ]))

    (ICC_DIR / "gray_gamma22_v2.icc").write_bytes(build_profile(bytes([2, 0x10, 0, 0]), "mntr", "GRAY", "XYZ ", [
        ("desc", tag_v2_desc("Gray Gamma 2.2")),
        ("wtpt", tag_xyz(D50)),
        ("kTRC", tag_gamma_curve(2.2)),
    ]))

    for source_name, target_name in (("sRGB.icc", "srgb_colord.icc"), ("AdobeRGB1998.icc", "adobe_rgb_colord.icc"),
                                     ("ProPhotoRGB.icc", "prophoto_rgb_colord.icc"), ("Rec709.icc", "rec709_colord.icc")):
        shutil.copyfile(COLORD_DIR / source_name, ICC_DIR / target_name)
    write_d50_profiles()


def exif_with_orientation(orientation):
    exif = Image.Exif()
    exif[EXIF_ORIENTATION_TAG] = orientation
    return exif.tobytes()


def write_images():
    source = Image.open(DATA_DIR / "testsrc_64x48.png").convert("RGB")
    display_p3 = (ICC_DIR / "display_p3_v4.icc").read_bytes()
    display_p3_sampled = (ICC_DIR / "display_p3_sampled_v4.icc").read_bytes()
    adobe_rgb = (ICC_DIR / "adobe_rgb_colord.icc").read_bytes()

    source.save(DATA_DIR / "testsrc_64x48_p3_orientation6.jpg", quality=90, icc_profile=display_p3,
                exif=exif_with_orientation(6))
    source.save(DATA_DIR / "testsrc_64x48_p3_sampled.jpg", quality=90, icc_profile=display_p3_sampled)
    source.save(DATA_DIR / "testsrc_64x48_422_orientation6.jpg", quality=90, subsampling=1,
                exif=exif_with_orientation(6))
    source.save(DATA_DIR / "testsrc_64x48_lossy_adobe_rgb_orientation8.webp", quality=90, icc_profile=adobe_rgb,
                exif=exif_with_orientation(8))
    source.save(DATA_DIR / "testsrc_64x48_p3.png", icc_profile=display_p3)

    # Every non-trivial orientation, each with Pillow's upright rendering as the reference.
    for orientation in range(2, 9):
        stored = DATA_DIR / f"testsrc_64x48_orientation{orientation}.png"
        source.save(stored, exif=exif_with_orientation(orientation))
        ImageOps.exif_transpose(Image.open(stored)).save(DATA_DIR / f"testsrc_64x48_orientation{orientation}_upright.png")


def write_avif():
    with tempfile.TemporaryDirectory() as scratch:
        y4m = Path(scratch) / "testsrc_128x96.y4m"
        subprocess.run([FFMPEG, "-v", "error", "-f", "lavfi", "-i", "testsrc=size=128x96:rate=1", "-frames:v", "1",
                        "-pix_fmt", "yuv444p", "-y", str(y4m)], check=True)
        common = ["avifenc", "--speed", "10", "--min", "0", "--max", "0", "--yuv", "444"]
        icc = str(ICC_DIR / "display_p3_v4.icc")
        # irot 1 is 90 degrees anticlockwise, irot 3 is 270 anticlockwise; imir 0 mirrors top to bottom. avifenc
        # 1.0.4 ignores --icc for a grid, so the grid fixture carries no profile.
        runs = [
            (["--icc", icc, "--irot", "1"], "testsrc_128x96_p3_irot1.avif"),
            (["--imir", "0"], "testsrc_128x96_imir0.avif"),
            (["--grid", "2x1", "--irot", "3"], "testsrc_128x96_grid_irot3.avif"),
        ]
        for arguments, name in runs:
            subprocess.run([*common, *arguments, str(y4m), str(DATA_DIR / name)], check=True, stdout=subprocess.DEVNULL)


def main():
    logging.basicConfig(level=logging.INFO, format="%(message)s")
    write_profiles()
    write_images()
    write_avif()
    logger.info("fixtures written to %s", DATA_DIR)


if __name__ == "__main__":
    main()
