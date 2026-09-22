#include "lossylab/core/error.hpp"
#include "lossylab/env/build_info.hpp"
#include "lossylab/env/capabilities.hpp"

#include <algorithm>
#include <cassert>
#include <cstdlib>

using namespace lossylab;

// These tests check the *gating mechanism*, not which codecs a particular
// FFmpeg happens to ship. A capability this build lacks makes a test skip, not
// fail: the library's API covers the full design surface, and which parts of
// it a build can serve is a runtime fact that varies legitimately.

namespace
{
    void test_the_build_provides_something_to_work_with()
    {
        const Capabilities& caps = capabilities();

        assert(!caps.encoders().empty());
        assert(!caps.decoders().empty());
        assert(!caps.filters().empty());
    }

    void test_lookup_is_consistent_between_find_and_has()
    {
        const Capabilities& caps = capabilities();

        for (const CodecInfo& encoder : caps.encoders())
        {
            assert(caps.has_encoder(encoder.name));
            assert(caps.find_encoder(encoder.name) != nullptr);
        }

        assert(!caps.has_encoder("definitely-not-a-codec"));
        assert(caps.find_encoder("definitely-not-a-codec") == nullptr);
        assert(!caps.has_filter("definitely-not-a-filter"));
    }

    void test_selection_picks_a_candidate_this_build_actually_has()
    {
        const Capabilities& caps = capabilities();

        // Whatever gets selected must be present and must encode the codec asked
        // for, never something adjacent.
        for (const VideoCodec codec : all_video_codecs())
        {
            const CodecInfo* encoder = caps.select_encoder(codec, EncoderBackend::Software);
            if (encoder == nullptr)
            {
                continue;  // Not in this build; that is a legitimate state.
            }
            assert(caps.has_encoder(encoder->name));
            assert(encoder->is_encoder);

            const std::vector<std::string> candidates =
                encoder_candidates(codec, EncoderBackend::Software);
            assert(std::find(candidates.begin(), candidates.end(), encoder->name) !=
                   candidates.end());
        }
    }

    void test_selection_follows_the_candidate_preference_order()
    {
        const Capabilities& caps = capabilities();

        for (const VideoCodec codec : all_video_codecs())
        {
            const std::vector<std::string> candidates =
                encoder_candidates(codec, EncoderBackend::Software);
            const CodecInfo* selected = caps.select_encoder(codec, EncoderBackend::Software);

            // Everything ahead of the selection in the preference list must be
            // absent, or the wrong encoder was chosen.
            for (const std::string& candidate : candidates)
            {
                if (selected != nullptr && candidate == selected->name)
                {
                    break;
                }
                assert(!caps.has_encoder(candidate));
            }
        }
    }

    void test_requiring_an_absent_codec_names_it_and_the_build()
    {
        const Capabilities& caps = capabilities();

        // Find any codec this build cannot encode, to exercise the refusal path.
        // If the build has everything, there is nothing to check here.
        for (const VideoCodec codec : all_video_codecs())
        {
            if (caps.supports(codec))
            {
                continue;
            }
            try
            {
                static_cast<void>(caps.require_encoder(codec));
                assert(false && "require_encoder() returned for a codec the build lacks");
            }
            catch (const UnsupportedCapability& e)
            {
                assert(e.name().find(to_string(codec)) != std::string::npos);
                assert(e.build_id() == build_info().build_id);
                // The message has to stand alone in a log.
                assert(std::string(e.what()).find(build_info().build_id) != std::string::npos);
            }
            return;
        }
        std::exit(77);  // this build supports every video codec, so nothing refuses
    }

    void test_supports_agrees_with_require()
    {
        const Capabilities& caps = capabilities();

        for (const VideoCodec codec : all_video_codecs())
        {
            bool threw = false;
            try
            {
                static_cast<void>(caps.require_encoder(codec));
            }
            catch (const UnsupportedCapability&)
            {
                threw = true;
            }
            assert(caps.supports(codec) == !threw);
        }

        for (const ImageCodec codec : all_image_codecs())
        {
            bool threw = false;
            try
            {
                static_cast<void>(caps.require_encoder(codec));
            }
            catch (const UnsupportedCapability&)
            {
                threw = true;
            }
            assert(caps.supports(codec) == !threw);
        }
    }

    void test_swscale_is_always_available()
    {
        // swscale is part of FFmpeg proper rather than an external library, so
        // unlike zscale it is present in every build.
        const Capabilities& caps = capabilities();
        assert(caps.supports(ResizeBackend::Swscale));
        caps.require_resize_backend(ResizeBackend::Swscale);
    }

    void test_zscale_is_gated_on_the_build()
    {
        const Capabilities& caps = capabilities();
        if (!caps.supports(ResizeBackend::Zscale))
        {
            try
            {
                caps.require_resize_backend(ResizeBackend::Zscale);
                assert(false && "expected throw");
            }
            catch (const UnsupportedCapability&)
            {
            }
            std::exit(77);  // this build has no zscale (libzimg)
        }
        caps.require_resize_backend(ResizeBackend::Zscale);
    }

    void test_vmaf_is_gated_on_the_build()
    {
        const Capabilities& caps = capabilities();
        if (!caps.supports(Metric::Vmaf))
        {
            try
            {
                caps.require_metric(Metric::Vmaf);
                assert(false && "expected throw");
            }
            catch (const UnsupportedCapability&)
            {
            }
            std::exit(77);  // this build has no libvmaf
        }
        caps.require_metric(Metric::Vmaf);
    }

    void test_an_encoder_declares_pixel_formats_it_accepts()
    {
        const Capabilities& caps = capabilities();

        const CodecInfo* encoder = caps.select_encoder(VideoCodec::H264, EncoderBackend::Software);
        if (encoder == nullptr)
        {
            std::exit(77);  // no software H.264 encoder in this build
        }

        // Every video encoder the library would drive has to accept 4:2:0 8-bit,
        // and must reject a format nothing supports.
        assert(!encoder->pixel_formats.empty());
        assert(encoder->accepts(PixelFormat::from_name("yuv420p")));
        assert(!encoder->accepts(PixelFormat::from_name("rgb48le")));
    }

    void test_an_encoder_exposes_its_option_schema()
    {
        const Capabilities& caps = capabilities();

        const CodecInfo* encoder = caps.select_encoder(VideoCodec::H264, EncoderBackend::Software);
        if (encoder == nullptr || encoder->name != "libx264")
        {
            std::exit(77);  // libx264 is not the selected H.264 encoder in this build
        }

        // The schema is what lets a pipeline spec be validated before it runs
        // rather than failing partway through a dataset.
        assert(!encoder->options.empty());
        assert(encoder->find_option("crf") != nullptr);
        assert(encoder->find_option("preset") != nullptr);
        assert(encoder->find_option("not-an-option") == nullptr);

        const OptionSchema* preset = encoder->find_option("preset");
        assert(!preset->type.empty());
    }

    void test_hardware_types_are_listed_without_being_opened()
    {
        // Listing must stay pure: opening a device loads vendor drivers and can
        // start threads, which would break the promise that a forked dataloader
        // worker is safe.
        const Capabilities& caps = capabilities();
        for (const HardwareDeviceInfo& device : caps.hardware_devices())
        {
            assert(!device.name.empty());
            assert(caps.has_hardware_device(device.name));
        }
        assert(!caps.has_hardware_device("not-a-device-type"));
        assert(!hardware_device_usable("not-a-device-type"));
    }

    void test_the_capability_summary_serializes()
    {
        const json::Value document = capabilities().to_json();

        assert(document.at("video_codecs").size() == all_video_codecs().size());
        assert(document.at("image_codecs").size() == all_image_codecs().size());
        assert(document.at("encoder_count").get<std::int64_t>() > 0);

        assert(json::parse(document.dump()) == document);
    }

    void test_capabilities_are_cached_not_recomputed()
    {
        // Every gate consults this, so it must not re-enumerate FFmpeg each time.
        assert(&capabilities() == &capabilities());
    }
}

int main()
{
    test_the_build_provides_something_to_work_with();
    test_lookup_is_consistent_between_find_and_has();
    test_selection_picks_a_candidate_this_build_actually_has();
    test_selection_follows_the_candidate_preference_order();
    test_requiring_an_absent_codec_names_it_and_the_build();
    test_supports_agrees_with_require();
    test_swscale_is_always_available();
    test_zscale_is_gated_on_the_build();
    test_vmaf_is_gated_on_the_build();
    test_an_encoder_declares_pixel_formats_it_accepts();
    test_an_encoder_exposes_its_option_schema();
    test_hardware_types_are_listed_without_being_opened();
    test_the_capability_summary_serializes();
    test_capabilities_are_cached_not_recomputed();
}
