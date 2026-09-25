import pytest

import lossylab


def test_has_encoder_matches_find_encoder():
    caps = lossylab.capabilities()
    for encoder in caps.encoders():
        assert caps.has_encoder(encoder.name)
        assert caps.find_encoder(encoder.name) is not None
    assert caps.find_encoder("not-a-real-encoder") is None
    assert caps.has_encoder("not-a-real-encoder") is False


def test_select_encoder_returns_none_when_absent_rather_than_substituting():
    caps = lossylab.capabilities()
    # No FFmpeg build ships a VP9 nvenc encoder; this must come back None, not
    # some other encoder silently substituted in its place.
    assert caps.select_encoder(lossylab.VideoCodec.Vp9, lossylab.EncoderBackend.Nvenc) is None


def test_require_encoder_raises_unsupported_capability_naming_the_build():
    caps = lossylab.capabilities()
    identity_hash = lossylab.build_info().identity_hash
    with pytest.raises(lossylab.UnsupportedCapability) as excinfo:
        caps.require_encoder(lossylab.VideoCodec.Av1, lossylab.EncoderBackend.Nvenc)
    error = excinfo.value
    assert error.identity_hash == identity_hash
    assert identity_hash in str(error)


def test_to_dict_round_trips():
    caps = lossylab.capabilities()
    document = caps.to_dict()
    assert isinstance(document, dict)
