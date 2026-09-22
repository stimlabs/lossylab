import json

import lossylab


def test_build_info_basic_fields():
    info = lossylab.build_info()
    assert len(info.libraries) == 6
    for library in info.libraries:
        assert library.matches_compiled()
    assert len(info.build_id) == 16
    assert info.license != lossylab.License.Unknown


def test_permits_proprietary_distribution_is_false_for_restrictive_licenses():
    for license_ in (lossylab.License.Gpl2, lossylab.License.Gpl3, lossylab.License.Nonfree):
        assert lossylab.permits_proprietary_distribution(license_) is False


def test_external_libraries_reported_without_enable_prefix():
    info = lossylab.build_info()
    for name in info.external_libraries:
        assert not name.startswith("--enable-")
        assert info.has_external_library(name)


def test_to_dict_round_trips_through_json():
    info = lossylab.build_info()
    document = info.to_dict()
    assert json.loads(json.dumps(document)) == document
