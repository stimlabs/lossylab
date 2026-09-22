#include "lossylab/core/error.hpp"
#include "lossylab/core/strict.hpp"

#include <cassert>

using namespace lossylab;

namespace
{
    void test_refuse_is_the_default_behavior()
    {
        ConversionList conversions;
        try
        {
            record_or_refuse(Strict::Refuse, conversions, "pix_fmt", "rgb24", "yuv420p",
                             ConversionCause::CodecConstraint, "libx264", "encode_video");
            assert(false && "expected ConversionRefused");
        }
        catch (const ConversionRefused&)
        {
        }
        assert(conversions.empty());
    }

    void test_refusal_names_what_it_refused()
    {
        try
        {
            ConversionList conversions;
            record_or_refuse(Strict::Refuse, conversions, "color_range", "limited", "full",
                             ConversionCause::GraphNegotiation, "scale", "FilterGraph::run");
            assert(false && "expected ConversionRefused");
        }
        catch (const ConversionRefused& e)
        {
            assert(e.from().find("limited") != std::string::npos);
            assert(e.to().find("full") != std::string::npos);
            assert(e.context() == std::string("FilterGraph::run"));
            // The message has to be actionable on its own.
            assert(std::string(e.what()).find("AllowRecorded") != std::string::npos);
        }
    }

    void test_allow_recorded_records_the_conversion()
    {
        ConversionList conversions;
        record_or_refuse(Strict::AllowRecorded, conversions, "pix_fmt", "rgb24", "yuv420p",
                         ConversionCause::CodecConstraint, "libx264", "encode_video");

        assert(conversions.size() == std::size_t{1});
        assert(conversions[0].property == std::string("pix_fmt"));
        assert(conversions[0].from == std::string("rgb24"));
        assert(conversions[0].to == std::string("yuv420p"));
        assert(conversions[0].cause == ConversionCause::CodecConstraint);
        assert(conversions[0].performed_by == std::string("libx264"));
    }

    void test_a_no_op_conversion_is_neither_refused_nor_recorded()
    {
        // Stages call this unconditionally, so equal endpoints must be free even in
        // Refuse mode. Otherwise strict mode would reject correct pipelines.
        ConversionList conversions;
        record_or_refuse(Strict::Refuse, conversions, "pix_fmt", "yuv420p", "yuv420p",
                         ConversionCause::Requested, "swscale", "convert");
        assert(conversions.empty());
    }

    void test_conversions_accumulate_in_order()
    {
        ConversionList conversions;
        record_or_refuse(Strict::AllowRecorded, conversions, "pix_fmt", "rgb24", "yuv444p",
                         ConversionCause::Requested, "swscale", "convert");
        record_or_refuse(Strict::AllowRecorded, conversions, "pix_fmt", "yuv444p", "yuv420p",
                         ConversionCause::Requested, "swscale", "convert");

        assert(conversions.size() == std::size_t{2});
        assert(conversions[0].to == std::string("yuv444p"));
        assert(conversions[1].from == std::string("yuv444p"));
    }

    void test_conversion_event_json_round_trip()
    {
        const ConversionEvent event{"chroma_location", "left", "center",
                                    ConversionCause::GraphNegotiation, "zscale"};
        assert(ConversionEvent::from_json(event.to_json()) == event);
    }

    void test_enum_names_round_trip()
    {
        for (const Strict mode : {Strict::Refuse, Strict::AllowRecorded})
        {
            assert(strict_from_string(to_string(mode)) == mode);
        }
        for (const ConversionCause cause : {ConversionCause::Requested,
                                            ConversionCause::CodecConstraint,
                                            ConversionCause::GraphNegotiation})
        {
            assert(conversion_cause_from_string(to_string(cause)) == cause);
        }

        try { (void)strict_from_string("permissive"); assert(false && "expected throw"); }
        catch (const ConfigError&) {}

        try { (void)conversion_cause_from_string("because"); assert(false && "expected throw"); }
        catch (const ConfigError&) {}
    }
}

int main()
{
    test_refuse_is_the_default_behavior();
    test_refusal_names_what_it_refused();
    test_allow_recorded_records_the_conversion();
    test_a_no_op_conversion_is_neither_refused_nor_recorded();
    test_conversions_accumulate_in_order();
    test_conversion_event_json_round_trip();
    test_enum_names_round_trip();
}
