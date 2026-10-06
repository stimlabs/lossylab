#include "lossylab/convert/convert.hpp"
#include "lossylab/core/error.hpp"
#include "lossylab/core/pipeline_spec.hpp"
#include "lossylab/env/capabilities.hpp"
#include "lossylab/io/decode_image.hpp"
#include "lossylab/transform/transform.hpp"

#include <cassert>
#include <string>

using namespace lossylab;

namespace
{
    std::string data_path(const std::string_view name)
    {
        return std::string(LOSSYLAB_TEST_DATA_DIR) + "/" + std::string(name);
    }

    ConvertOptions to_srgb24()
    {
        ConvertOptions options;
        options.pixel_format = PixelFormat::from_name("rgb24");
        options.color = ColorSpec::srgb();
        return options;
    }

    RoundtripImageConfiguration jpeg_roundtrip()
    {
        RoundtripImageConfiguration configuration;
        configuration.encode.codec = ImageCodec::Mjpeg;
        configuration.encode.pixel_format = PixelFormat::from_name("yuvj420p");
        configuration.encode.rate_control = RateControl::quality(2);
        configuration.decode.conversion = to_srgb24();
        return configuration;
    }

    /// The equalization chain without its decode, for a frame.
    PipelineSpec frame_spec()
    {
        PipelineSpec spec;
        spec.add(to_srgb24(), "to sRGB")
            .add(CropOptions{16, 16, 32, 16})
            .add(OrientOptions{6, Strict::AllowRecorded})
            .add(AchromaticOptions{})
            .add(jpeg_roundtrip());
        return spec;
    }

    template <typename Exception, typename Function>
    std::string expect_throw(Function&& function)
    {
        try
        {
            function();
        }
        catch (const Exception& error)
        {
            return error.what();
        }
        assert(false && "expected throw");
        return {};
    }

    void test_a_spec_round_trips_through_json()
    {
        PipelineSpec spec = frame_spec();
        spec.set_source_sha256("sha256:00");
        const json::Value document = spec.to_json();
        assert(document.at("stages").at(0).at("kind") == "convert");
        assert(document.at("stages").at(0).at("label") == "to sRGB");
        assert(document.at("stages").at(1).at("configuration").at("width") == 32);
        assert(!document.at("stages").at(1).contains("label"));

        const PipelineSpec parsed = PipelineSpec::from_json(document);
        assert(parsed == spec);
        assert(parsed.stages()[2].kind() == StageKind::Orient);
        assert(std::get<CropOptions>(parsed.stages()[1].configuration).x == 16);
        assert(parsed.source_sha256() == std::optional<std::string>("sha256:00"));
        assert(PipelineSpec::parse(document.dump()) == spec);
    }

    void test_the_spec_id_is_a_sha256_of_everything_the_spec_says()
    {
        const PipelineSpec spec = frame_spec();
        assert(spec.spec_id().starts_with("sha256:") && spec.spec_id().size() == 7 + 64);
        assert(spec.spec_id() == frame_spec().spec_id());

        PipelineSpec moved;
        moved.add(to_srgb24(), "to sRGB")
            .add(CropOptions{0, 16, 32, 16})
            .add(OrientOptions{6, Strict::AllowRecorded})
            .add(AchromaticOptions{})
            .add(jpeg_roundtrip());
        assert(moved.spec_id() != spec.spec_id());

        PipelineSpec sourced = frame_spec();
        sourced.set_source_sha256("sha256:00");
        assert(sourced.spec_id() != spec.spec_id());

        PipelineSpec reordered;
        reordered.add(AchromaticOptions{}).add(to_srgb24());
        PipelineSpec ordered;
        ordered.add(to_srgb24()).add(AchromaticOptions{});
        assert(reordered.spec_id() != ordered.spec_id());
    }

    void test_validation_names_the_offending_stage()
    {
        expect_throw<ConfigError>([] { PipelineSpec().validate(); });

        PipelineSpec late_decode = frame_spec();
        late_decode.add(DecodeImageOptions{}, "second decode");
        const std::string message = expect_throw<ConfigError>([&] { late_decode.validate(); });
        assert(message.find("pipeline stage 5 (decode_image 'second decode')") != std::string::npos);

        PipelineSpec measuring;
        measuring.add(MeasureOptions{});
        assert(expect_throw<ConfigError>([&] { measuring.validate(); }).find("does not run in a pipeline") !=
               std::string::npos);

        PipelineSpec decode_first;
        decode_first.add(DecodeImageOptions{}).add(to_srgb24());
        decode_first.validate();
    }

    void test_validation_checks_what_the_build_can_do()
    {
        for (const ImageCodec codec : all_image_codecs())
        {
            if (capabilities().supports(codec))
            {
                continue;
            }
            RoundtripImageConfiguration configuration = jpeg_roundtrip();
            configuration.encode.codec = codec;
            PipelineSpec spec;
            spec.add(configuration);
            expect_throw<UnsupportedCapability>([&] { spec.validate(); });
            break;
        }

        ConvertOptions zscale = to_srgb24();
        zscale.backend = ResizeBackend::Zscale;
        PipelineSpec spec;
        spec.add(zscale);
        if (capabilities().supports(ResizeBackend::Zscale))
        {
            spec.validate();
        }
        else
        {
            expect_throw<UnsupportedCapability>([&] { spec.validate(); });
        }
    }

    void test_a_record_turns_back_into_its_spec()
    {
        const Source source = Source::from_path(data_path("testsrc_64x48_q75.jpg"));
        DecodeImageOptions decode_options;
        decode_options.conversion = to_srgb24();
        const DecodedImage decoded = decode_image(source, decode_options);
        ProcessingRecord record = decoded.processing_record();
        const FrameResult cropped = crop(decoded.frame, CropOptions{0, 0, 32, 32});
        record.append(cropped.record, cropped.configuration);

        const PipelineSpec spec = PipelineSpec::from_record(record);
        assert(spec.size() == 2);
        assert(spec.stages()[0].kind() == StageKind::DecodeImage);
        assert(spec.stages()[1].kind() == StageKind::Crop);
        assert(spec.source_sha256() == std::optional<std::string>(source.sha256()));
        spec.validate();
    }
}

int main()
{
    test_a_spec_round_trips_through_json();
    test_the_spec_id_is_a_sha256_of_everything_the_spec_says();
    test_validation_names_the_offending_stage();
    test_validation_checks_what_the_build_can_do();
    test_a_record_turns_back_into_its_spec();
}
