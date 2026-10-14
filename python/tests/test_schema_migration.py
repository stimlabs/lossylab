import json
from pathlib import Path

import pytest

import lossylab

RECORDS_DIR = Path(__file__).resolve().parents[2] / "tests" / "data" / "records"


def _schema_14_record():
    return json.loads((RECORDS_DIR / "record_schema14_image.json").read_text())


def test_a_schema_14_record_loads_and_is_written_back_in_the_current_schema():
    stored = _schema_14_record()
    assert stored["schema_version"] == 14

    record = lossylab.ProcessingRecord.from_dict(stored)

    written = record.to_dict()
    assert written["schema_version"] > 14
    assert lossylab.ProcessingRecord.from_dict(written).to_dict() == written
    assert record.stages()[2].block_grid.kind == lossylab.BlockGridKind.JpegMcu


def test_a_schema_version_that_is_not_read_raises_schema_version_error():
    stored = _schema_14_record()
    stored["schema_version"] = 13

    with pytest.raises(lossylab.SchemaVersionError):
        lossylab.ProcessingRecord.from_dict(stored)


def test_schema_version_error_is_a_config_error():
    assert issubclass(lossylab.SchemaVersionError, lossylab.ConfigError)


def test_a_file_error_checks_the_version_too():
    error = lossylab.capture_probe(lossylab.Source.from_bytes(b"\x5a" * 256)).error()
    stored = error.to_dict()
    assert lossylab.FileError.from_dict(stored) == error

    stored["schema_version"] = 1
    with pytest.raises(lossylab.SchemaVersionError):
        lossylab.FileError.from_dict(stored)
