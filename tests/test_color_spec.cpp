#include "lossylab/core/color_spec.hpp"
#include "lossylab/core/error.hpp"

#include <cassert>

using namespace lossylab;

namespace
{
    void test_presets_are_fully_specified()
    {
        const ColorSpec specs[] = {
            ColorSpec::bt709_limited(), ColorSpec::bt709_full(),
            ColorSpec::bt601_limited(), ColorSpec::bt601_full(),
            ColorSpec::smpte170m_limited(), ColorSpec::bt2020_ncl_limited(),
            ColorSpec::pq_bt2020(), ColorSpec::hlg_bt2020(),
            ColorSpec::srgb(), ColorSpec::jpeg(),
        };
        for (const ColorSpec& spec : specs)
        {
            assert(spec.is_fully_specified());
            spec.require_fully_specified("test");
        }
    }

    void test_a_default_constructed_spec_is_not_usable()
    {
        const ColorSpec spec;
        assert(!spec.is_fully_specified());
        try { spec.require_fully_specified("convert"); assert(false && "expected throw"); }
        catch (const ConfigError&) {}
    }

    void test_the_error_names_every_missing_field()
    {
        ColorSpec spec = ColorSpec::bt709_limited();
        spec.matrix = ColorMatrix::Unspecified;
        spec.transfer = TransferCharacteristic::Unspecified;

        try
        {
            spec.require_fully_specified("convert");
            assert(false && "expected ConfigError");
        }
        catch (const ConfigError& e)
        {
            const std::string message = e.what();
            assert(message.find("matrix") != std::string::npos);
            assert(message.find("transfer") != std::string::npos);
            assert(message.find("convert") != std::string::npos);
            // Fields that were set must not be listed.
            assert(message.find("primaries") == std::string::npos);
        }
    }

    void test_rgb_needs_no_chroma_siting()
    {
        // RGB has no chroma planes, so an unspecified siting is absent rather than
        // missing. Requiring it would make every RGB conversion fail.
        const ColorSpec spec = ColorSpec::srgb();
        assert(spec.is_rgb());
        assert(spec.chroma_location == ChromaLocation::Unspecified);
        assert(spec.is_fully_specified());
    }

    void test_yuv_requires_chroma_siting()
    {
        ColorSpec spec = ColorSpec::bt709_limited();
        spec.chroma_location = ChromaLocation::Unspecified;
        assert(!spec.is_fully_specified());
    }

    void test_defaults_fill_only_unspecified_fields()
    {
        ColorSpec partial;
        partial.matrix = ColorMatrix::Bt2020Ncl;

        const ColorSpec filled = partial.with_defaults_from(ColorSpec::bt709_limited());

        // The field that was set survives.
        assert(filled.matrix == ColorMatrix::Bt2020Ncl);
        // The rest come from the fallback.
        assert(filled.range == ColorRange::Limited);
        assert(filled.primaries == ColorPrimaries::Bt709);
        assert(filled.transfer == TransferCharacteristic::Bt709);
        assert(filled.chroma_location == ChromaLocation::Left);
    }

    void test_the_mix_ups_the_library_models_are_distinguishable()
    {
        // BT.601 vs BT.709 and limited vs full are exactly the confusions the
        // library reproduces as augmentation and looks for as traces, so they must
        // never compare equal.
        assert(ColorSpec::bt709_limited() != ColorSpec::bt601_limited());
        assert(ColorSpec::bt709_limited() != ColorSpec::bt709_full());
        assert(ColorSpec::bt601_limited() != ColorSpec::jpeg());
    }

    void test_json_round_trip_preserves_every_field()
    {
        const ColorSpec specs[] = {
            ColorSpec::bt709_limited(), ColorSpec::jpeg(), ColorSpec::pq_bt2020(),
            ColorSpec::hlg_bt2020(), ColorSpec::srgb(), ColorSpec(),
        };
        for (const ColorSpec& spec : specs)
        {
            assert(ColorSpec::from_json(spec.to_json()) == spec);
        }
    }

    void test_describe_is_stable_and_readable()
    {
        const std::string described = ColorSpec::bt709_limited().describe();
        assert(described.find("bt709") != std::string::npos);
        assert(described.find("left") != std::string::npos);
        // Same input, same label: records are compared across runs.
        assert(described == ColorSpec::bt709_limited().describe());
    }

    void test_enum_names_round_trip()
    {
        assert(color_matrix_from_string(to_string(ColorMatrix::Bt2020Ncl)) ==
               ColorMatrix::Bt2020Ncl);
        assert(color_primaries_from_string(to_string(ColorPrimaries::Smpte432)) ==
               ColorPrimaries::Smpte432);
        assert(transfer_from_string(to_string(TransferCharacteristic::Smpte2084)) ==
               TransferCharacteristic::Smpte2084);
        assert(chroma_location_from_string(to_string(ChromaLocation::Center)) ==
               ChromaLocation::Center);

        for (const ColorRange range : {ColorRange::Limited, ColorRange::Full})
        {
            assert(color_range_from_string(to_string(range)) == range);
        }
    }

    void test_range_accepts_both_spellings()
    {
        // FFmpeg says tv/pc; the library says limited/full. Both have to work,
        // because specs get written by hand.
        assert(color_range_from_string("limited") == ColorRange::Limited);
        assert(color_range_from_string("tv") == ColorRange::Limited);
        assert(color_range_from_string("full") == ColorRange::Full);
        assert(color_range_from_string("pc") == ColorRange::Full);
    }

    void test_unknown_names_are_rejected()
    {
        try { (void)color_matrix_from_string("bt709ish"); assert(false && "expected throw"); }
        catch (const ConfigError&) {}

        try { (void)color_range_from_string("partial"); assert(false && "expected throw"); }
        catch (const ConfigError&) {}

        try { (void)color_primaries_from_string("nonsense"); assert(false && "expected throw"); }
        catch (const ConfigError&) {}

        try { (void)transfer_from_string("nonsense"); assert(false && "expected throw"); }
        catch (const ConfigError&) {}

        try { (void)chroma_location_from_string("middle-ish"); assert(false && "expected throw"); }
        catch (const ConfigError&) {}
    }
}

int main()
{
    test_presets_are_fully_specified();
    test_a_default_constructed_spec_is_not_usable();
    test_the_error_names_every_missing_field();
    test_rgb_needs_no_chroma_siting();
    test_yuv_requires_chroma_siting();
    test_defaults_fill_only_unspecified_fields();
    test_the_mix_ups_the_library_models_are_distinguishable();
    test_json_round_trip_preserves_every_field();
    test_describe_is_stable_and_readable();
    test_enum_names_round_trip();
    test_range_accepts_both_spellings();
    test_unknown_names_are_rejected();
}
