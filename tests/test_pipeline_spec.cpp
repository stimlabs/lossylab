#include "lossylab/core/error.hpp"
#include "lossylab/core/pipeline_spec.hpp"
#include "lossylab/env/capabilities.hpp"

#include <cassert>
#include <cstdlib>

using namespace lossylab;

namespace
{
    struct AbsentEncoder
    {
        VideoCodec codec = VideoCodec::H264;
        EncoderBackend backend = EncoderBackend::Software;
    };

    /// A codec and backend pair this build cannot encode, software first. Some
    /// pairs have no encoder in any build (NVENC has no VP9 encoder), so one
    /// always exists.
    AbsentEncoder absent_video_encoder()
    {
        for (const EncoderBackend backend : {EncoderBackend::Software, EncoderBackend::Vaapi,
                                             EncoderBackend::Nvenc, EncoderBackend::Qsv,
                                             EncoderBackend::VideoToolbox})
        {
            for (const VideoCodec codec : all_video_codecs())
            {
                if (!capabilities().supports(codec, backend))
                {
                    return {codec, backend};
                }
            }
        }
        assert(false && "this build can encode every codec on every backend");
        return {};
    }

    StageSpec convert_stage()
    {
        StageSpec stage;
        stage.kind = StageKind::Convert;
        stage.params = json::object({{"pix_fmt", "yuv420p"}});
        return stage;
    }

    StageSpec encode_stage(const char* codec)
    {
        StageSpec stage;
        stage.kind = StageKind::EncodeVideo;
        stage.params = json::object({{"codec", codec}, {"pix_fmt", "yuv420p"}});
        return stage;
    }

    void test_a_spec_round_trips_through_json()
    {
        PipelineSpec spec;
        spec.add(convert_stage())
            .add(StageKind::Resize, json::object({{"kernel", "lanczos"}}), "downscale")
            .add(encode_stage("h264"));

        const PipelineSpec parsed = PipelineSpec::from_json(spec.to_json());

        assert(parsed.size() == std::size_t{3});
        assert(parsed == spec);
        assert(parsed.stages()[1].label == std::string("downscale"));
    }

    void test_a_spec_parses_from_text()
    {
        const PipelineSpec spec = PipelineSpec::parse(R"({
            "stages": [
                {"kind": "convert", "params": {"pix_fmt": "yuv420p"}},
                {"kind": "resize", "params": {"kernel": "bicubic"}, "label": "half"}
            ]
        })");

        assert(spec.size() == std::size_t{2});
        assert(spec.stages()[0].kind == StageKind::Convert);
        assert(spec.stages()[1].label == std::string("half"));
    }

    void test_the_spec_id_identifies_the_pipeline_not_the_run()
    {
        PipelineSpec first;
        first.add(convert_stage()).add(encode_stage("h264"));

        PipelineSpec same;
        same.add(convert_stage()).add(encode_stage("h264"));

        PipelineSpec reordered;
        reordered.add(encode_stage("h264")).add(convert_stage());

        // Two datasets built from the same spec must share an id, and order has to
        // matter: convert-then-encode is not encode-then-convert.
        assert(first.spec_id() == same.spec_id());
        assert(first.spec_id() != reordered.spec_id());
        assert(first.spec_id().size() == std::size_t{16});
    }

    void test_a_changed_parameter_changes_the_spec_id()
    {
        PipelineSpec crf23;
        crf23.add(StageKind::EncodeVideo,
                  json::object({{"codec", "h264"}, {"pix_fmt", "yuv420p"}, {"crf", 23}}));

        PipelineSpec crf30;
        crf30.add(StageKind::EncodeVideo,
                  json::object({{"codec", "h264"}, {"pix_fmt", "yuv420p"}, {"crf", 30}}));

        assert(crf23.spec_id() != crf30.spec_id());
    }

    void test_an_empty_spec_is_rejected()
    {
        try { PipelineSpec().validate(); assert(false && "expected throw"); } catch (const ConfigError&) {}
    }

    void test_validation_catches_an_unknown_pixel_format()
    {
        PipelineSpec spec;
        spec.add(StageKind::Convert, json::object({{"pix_fmt", "yuv420q"}}));

        try { spec.validate(); assert(false && "expected throw"); } catch (const ConfigError&) {}
    }

    void test_validation_catches_an_unknown_kernel()
    {
        PipelineSpec spec;
        spec.add(StageKind::Resize, json::object({{"kernel", "mitchellish"}}));

        try { spec.validate(); assert(false && "expected throw"); } catch (const ConfigError&) {}
    }

    void test_validation_catches_a_missing_required_parameter()
    {
        PipelineSpec spec;
        spec.add(StageKind::EncodeVideo, json::object({{"pix_fmt", "yuv420p"}}));

        try { spec.validate(); assert(false && "expected throw"); } catch (const ConfigError&) {}
    }

    void test_the_error_names_the_offending_stage()
    {
        PipelineSpec spec;
        spec.add(convert_stage())
            .add(StageKind::Resize, json::object({{"kernel", "nonsense"}}), "downscale");

        try
        {
            spec.validate();
            assert(false && "expected validation to fail");
        }
        catch (const ConfigError& e)
        {
            const std::string message = e.what();
            // Finding the bad stage in a long randomized pipeline should not
            // require guessing.
            assert(message.find("stage 1") != std::string::npos);
            assert(message.find("downscale") != std::string::npos);
        }
    }

    void test_validation_rejects_a_codec_this_build_lacks()
    {
        const AbsentEncoder absent = absent_video_encoder();
        StageSpec encode;
        encode.kind = StageKind::EncodeVideo;
        encode.params = json::object({{"codec", to_string(absent.codec)},
                                      {"backend", to_string(absent.backend)},
                                      {"pix_fmt", "yuv420p"}});

        PipelineSpec spec;
        spec.add(convert_stage()).add(encode);

        // This is the case the whole gate exists for: catching it here rather
        // than partway through building a dataset, with half the samples
        // already written under a different distribution.
        try { spec.validate(); assert(false && "expected throw"); } catch (const UnsupportedCapability&) {}
    }

    void test_validation_accepts_a_codec_this_build_has()
    {
        const Capabilities& caps = capabilities();

        for (const VideoCodec codec : all_video_codecs())
        {
            if (!caps.supports(codec))
            {
                continue;
            }

            PipelineSpec spec;
            spec.add(convert_stage()).add(encode_stage(to_string(codec).c_str()));
            spec.validate();
            return;
        }
        // TODO: exit(77) also skips every later test in this file; skip only this test.
        std::exit(77);  // skip: this build supports no video codecs at all
    }

    void test_validation_gates_the_resize_backend()
    {
        PipelineSpec spec;
        spec.add(StageKind::Resize, json::object({{"backend", "swscale"}}));
        spec.validate();

        PipelineSpec zscale;
        zscale.add(StageKind::Resize, json::object({{"backend", "zscale"}}));
        if (!capabilities().supports(ResizeBackend::Zscale))
        {
            try { zscale.validate(); assert(false && "expected throw"); } catch (const UnsupportedCapability&) {}
        }
        else
        {
            zscale.validate();
        }
    }

    void test_stages_with_nothing_build_dependent_always_validate()
    {
        PipelineSpec spec;
        spec.add(StageKind::Reinterpret, json::object({{"as_color", "bt601"}}))
            .add(StageKind::AnimateStill, json::object({{"frame_count", 25}}))
            .add(StageKind::DecodeImage, json::Value());

        spec.validate();
    }
}

int main()
{
    test_a_spec_round_trips_through_json();
    test_a_spec_parses_from_text();
    test_the_spec_id_identifies_the_pipeline_not_the_run();
    test_a_changed_parameter_changes_the_spec_id();
    test_an_empty_spec_is_rejected();
    test_validation_catches_an_unknown_pixel_format();
    test_validation_catches_an_unknown_kernel();
    test_validation_catches_a_missing_required_parameter();
    test_the_error_names_the_offending_stage();
    test_validation_rejects_a_codec_this_build_lacks();
    test_validation_accepts_a_codec_this_build_has();
    test_validation_gates_the_resize_backend();
    test_stages_with_nothing_build_dependent_always_validate();
}
