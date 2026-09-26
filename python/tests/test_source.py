import json
from pathlib import Path
from hashlib import sha256

import lossylab

DATA_DIR = Path(__file__).resolve().parents[2] / "tests" / "data"


def test_source_hash_is_sha256():
    path = DATA_DIR / "testsrc_64x48.png"
    bytes_ = path.read_bytes()

    path_source = lossylab.Source.from_path(str(path))

    expected_sha256 = "sha256:" + sha256(bytes_).hexdigest()
    assert path_source.sha256() == expected_sha256


def test_a_path_and_its_bytes_hash_the_same():
    path = DATA_DIR / "testsrc_64x48.png"
    bytes_ = path.read_bytes()

    from_path = lossylab.Source.from_path(str(path))
    from_bytes = lossylab.Source.from_bytes(bytes_)

    digest = from_path.sha256()
    assert digest.startswith("sha256:")
    assert len(digest) == len("sha256:") + 64
    assert digest == from_bytes.sha256()


def test_different_bytes_hash_differently():
    bytes_ = bytearray((DATA_DIR / "testsrc_64x48.png").read_bytes())
    original = lossylab.Source.from_bytes(bytes(bytes_))

    bytes_[-1] ^= 0xFF
    flipped = lossylab.Source.from_bytes(bytes(bytes_))

    assert original.sha256() != flipped.sha256()


def test_from_memory_hashes_like_from_bytes():
    bytes_ = (DATA_DIR / "testsrc_64x48.png").read_bytes()
    owned = lossylab.Source.from_bytes(bytes_)
    borrowed = lossylab.Source.from_memory(bytes_)

    assert owned.sha256() == borrowed.sha256()


def test_decode_image_records_the_hash_and_not_the_path():
    path = DATA_DIR / "testsrc_64x48.png"
    source = lossylab.Source.from_path(str(path))

    result = lossylab.decode_image(source)
    params = result.record.params

    assert params["source_sha256"] == source.sha256()
    assert "source" not in params
    assert str(path) not in json.dumps(params)
