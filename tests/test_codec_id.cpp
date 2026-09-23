#include "lossylab/core/codec_id.hpp"
#include "lossylab/core/error.hpp"

#include <cassert>

using namespace lossylab;

namespace
{
    void test_image_codec_names_round_trip()
    {
        for (const ImageCodec codec : all_image_codecs())
        {
            assert(image_codec_from_string(to_string(codec)) == codec);
        }
    }

    void test_video_codec_names_round_trip()
    {
        for (const VideoCodec codec : all_video_codecs())
        {
            assert(video_codec_from_string(to_string(codec)) == codec);
        }
    }

    void test_common_aliases_are_accepted()
    {
        assert(video_codec_from_string("h265") == VideoCodec::Hevc);
        assert(video_codec_from_string("avc") == VideoCodec::H264);
        assert(image_codec_from_string("jpeg") == ImageCodec::Mjpeg);
        assert(image_codec_from_string("jpegxl") == ImageCodec::Jxl);
    }

    void test_unknown_codec_names_are_rejected()
    {
        try { (void)video_codec_from_string("mpeg2"); assert(false && "expected throw"); }
        catch (const ConfigError&) {}

        try { (void)image_codec_from_string("bmp"); assert(false && "expected throw"); }
        catch (const ConfigError&) {}

        try { (void)encoder_backend_from_string("cuda"); assert(false && "expected throw"); }
        catch (const ConfigError&) {}

        try { (void)resize_backend_from_string("pillow"); assert(false && "expected throw"); }
        catch (const ConfigError&) {}

        try { (void)metric_from_string("lpips"); assert(false && "expected throw"); }
        catch (const ConfigError&) {}
    }

    void test_every_codec_names_at_least_one_software_encoder()
    {
        // The enum is the full design surface, not whatever the local FFmpeg has.
        // Availability is a runtime question; a missing candidate list would be a
        // gap in the design itself.
        for (const VideoCodec codec : all_video_codecs())
        {
            assert(!encoder_candidates(codec, EncoderBackend::Software).empty());
            assert(!decoder_candidates(codec).empty());
        }
        for (const ImageCodec codec : all_image_codecs())
        {
            // FFmpeg has no HEIF muxer, so no encoder can write a HEIF file.
            assert(encoder_candidates(codec).empty() == (codec == ImageCodec::Heif));
            assert(!decoder_candidates(codec).empty());
        }
    }

    void test_av1_offers_several_encoders_in_preference_order()
    {
        // Several encoders implement AV1, so selection has to be a lookup against
        // the build rather than a single hardcoded name.
        const std::vector<std::string> candidates =
            encoder_candidates(VideoCodec::Av1, EncoderBackend::Software);
        assert(candidates.size() > 1);
        assert(candidates[0] == std::string("libsvtav1"));
    }

    void test_hardware_backends_are_named_per_codec()
    {
        assert(encoder_candidates(VideoCodec::H264, EncoderBackend::Nvenc)[0] ==
               std::string("h264_nvenc"));
        assert(encoder_candidates(VideoCodec::Hevc, EncoderBackend::Vaapi)[0] ==
               std::string("hevc_vaapi"));

        // Combinations that no hardware implements are empty, not invented.
        assert(encoder_candidates(VideoCodec::Vp9, EncoderBackend::Nvenc).empty());
    }

    void test_resize_backends_map_to_filter_names()
    {
        assert(filter_name(ResizeBackend::Swscale) == std::string("scale"));
        assert(filter_name(ResizeBackend::Zscale) == std::string("zscale"));
    }

    void test_metric_names_round_trip()
    {
        for (const Metric metric : {Metric::Psnr, Metric::Ssim, Metric::Vmaf})
        {
            assert(metric_from_string(to_string(metric)) == metric);
        }
    }
}

int main()
{
    test_image_codec_names_round_trip();
    test_video_codec_names_round_trip();
    test_common_aliases_are_accepted();
    test_unknown_codec_names_are_rejected();
    test_every_codec_names_at_least_one_software_encoder();
    test_av1_offers_several_encoders_in_preference_order();
    test_hardware_backends_are_named_per_codec();
    test_resize_backends_map_to_filter_names();
    test_metric_names_round_trip();
}
