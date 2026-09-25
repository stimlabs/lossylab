import json

import lossylab


def test_build_info_basic_fields():
    info = lossylab.build_info()
    assert len(info.ffmpeg.libraries) == 6
    for library in info.ffmpeg.libraries:
        assert library.matches_compiled()
    assert info.ffmpeg.license != lossylab.License.Unknown


def test_the_identity_names_the_commit_and_carries_a_sha256_hash():
    info = lossylab.build_info()
    assert len(info.lossylab.commit) == 40
    assert info.identity_hash.startswith("sha256:")
    assert len(info.identity_hash) == len("sha256:") + 64


def test_build_diff_names_what_differs():
    document = lossylab.build_info().to_dict()
    assert lossylab.build_diff(document, document) == {}
    other = json.loads(json.dumps(document))
    other["lossylab"]["commit"] = "0000"
    assert lossylab.build_diff(document, other) == {"lossylab.commit": [document["lossylab"]["commit"], "0000"]}


def test_diagnostics_describe_the_machine():
    assert lossylab.diagnostics().architecture
    assert lossylab.diagnostics().os


def test_a_record_carries_its_build_and_machine():
    record = lossylab.ProcessingRecord.for_this_build()
    assert record.build == lossylab.build_info().to_dict()
    assert record.diagnostics == lossylab.diagnostics().to_dict()


def test_permits_proprietary_distribution_is_false_for_restrictive_licenses():
    for license_ in (lossylab.License.Gpl2, lossylab.License.Gpl3, lossylab.License.Nonfree):
        assert lossylab.permits_proprietary_distribution(license_) is False


def test_the_ffmpeg_identity_is_compact():
    ffmpeg = lossylab.build_info().to_dict()["ffmpeg"]
    assert ffmpeg["configure_hash"].startswith("sha256:")
    assert "configuration" not in ffmpeg
    assert isinstance(ffmpeg["libraries"]["libavcodec"], str)
    assert len(json.dumps(ffmpeg)) < 500


def test_to_dict_round_trips_through_json():
    info = lossylab.build_info()
    document = info.to_dict()
    assert json.loads(json.dumps(document)) == document
