#include "lossylab/codec/encode.hpp"
#include "lossylab/core/error.hpp"
#include "lossylab/env/capabilities.hpp"
#include "lossylab/io/probe.hpp"
#include "lossylab/measure/measure.hpp"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

using namespace lossylab;

namespace
{
    /// A clip of a diagonal pattern moving two pixels right per frame, with
    /// chroma that varies across the frame.
    std::vector<Frame> moving_clip(const int frame_count, const int width = 64, const int height = 64)
    {
        std::vector<Frame> frames;
        for (int index = 0; index < frame_count; ++index)
        {
            Frame frame = Frame::allocate(width, height, PixelFormat::from_name("yuv420p"),
                                          ColorSpec::bt709_limited());
            for (int plane_index = 0; plane_index < frame.plane_count(); ++plane_index)
            {
                const PlaneView plane = frame.plane(plane_index);
                for (int y = 0; y < plane.height; ++y)
                {
                    for (int x = 0; x < plane.width; ++x)
                    {
                        const int value = plane_index == 0 ? 16 + ((x + y + 2 * index) * 7) % 220
                                                           : 64 + ((x * 3 + plane_index * y) % 128);
                        plane.row(y)[x] = static_cast<std::uint8_t>(value);
                    }
                }
            }
            frames.push_back(std::move(frame));
        }
        return frames;
    }

    EncodeVideoOptions video_options(const VideoCodec codec, const RateControl& rate_control)
    {
        EncodeVideoOptions options;
        options.codec = codec;
        options.pixel_format = PixelFormat::from_name("yuv420p");
        options.rate_control = rate_control;
        options.gop.b_frames = 0;
        return options;
    }

    bool starts_with(const std::vector<std::uint8_t>& bytes, const std::size_t offset, const std::string_view text)
    {
        return bytes.size() >= offset + text.size() &&
               std::memcmp(bytes.data() + offset, text.data(), text.size()) == 0;
    }

    template <typename Exception, typename Callable>
    void expect_throw(Callable&& callable)
    {
        try
        {
            callable();
            assert(false && "expected an exception");
        }
        catch (const Exception&)
        {
        }
    }

    std::vector<int> key_frame_indices(const StageRecord& record)
    {
        std::vector<int> indices;
        for (const FrameStats& stats : record.frames)
        {
            if (stats.key_frame)
            {
                indices.push_back(stats.index);
            }
        }
        return indices;
    }

    /// Checks the roundtrip returns every frame in the source's format, close
    /// to the source.
    void check_roundtrip(const std::vector<Frame>& clip, const EncodeVideoOptions& options, const double min_psnr)
    {
        ConvertOptions conversion;
        conversion.pixel_format = clip.front().pixel_format();
        conversion.color = clip.front().color();
        DecodeSpec decode_spec;
        decode_spec.conversion = conversion;
        const FramesResult result = roundtrip(clip, options, decode_spec);
        assert(result.frames.size() == clip.size());
        assert(result.record.kind() == StageKind::RoundtripVideo);
        const RoundtripVideoConfiguration& configuration = std::get<RoundtripVideoConfiguration>(result.configuration);
        assert(configuration.decode.frame_indices.size() == clip.size());
        for (const Frame& frame : result.frames)
        {
            assert(frame.describe() == clip.front().describe());
        }
        assert(compare(clip, result.frames, {Metric::Psnr}).evidence().pooled.at("psnr_mean") > min_psnr);
    }

    void test_h264_encodes_an_annex_b_stream_with_per_frame_statistics()
    {
        const std::vector<Frame> clip = moving_clip(10);
        const EncodeVideoOptions options = video_options(VideoCodec::H264, RateControl::crf(23));
        const EncodedResult encoded = encode_video(clip, options);

        assert(starts_with(encoded.bytes, 0, std::string_view("\0\0\0\1", 4)));
        const StageRecord& record = encoded.record;
        assert(record.kind() == StageKind::EncodeVideo);
        assert(record.implementation == "libx264");
        assert(std::get<EncodeVideoEvidence>(record.evidence).container == "h264");
        assert(record.frames.size() == clip.size());
        assert(record.frames.front().key_frame && record.frames.front().picture_type == PictureType::I);
        for (std::size_t index = 0; index < record.frames.size(); ++index)
        {
            assert(record.frames[index].index == static_cast<int>(index));
            assert(record.frames[index].qp_mean.has_value());
            assert(record.frames[index].picture_type != PictureType::B);
        }
        assert(record.block_grid->kind == BlockGridKind::Macroblock16);
        const EncodeVideoEvidence& evidence = std::get<EncodeVideoEvidence>(record.evidence);
        assert(evidence.resolved.options.at("crf") == "23.000000");
        assert(std::abs(evidence.achieved_bpp - encoded.bits_per_pixel()) < 1e-9);

        assert(encode_video(clip, options).bytes == encoded.bytes);
        check_roundtrip(clip, options, 30.0);
    }

    void test_the_gop_structure_places_keyframes_and_b_frames()
    {
        const std::vector<Frame> clip = moving_clip(24);
        EncodeVideoOptions options = video_options(VideoCodec::H264, RateControl::crf(23));
        options.gop.keyframe_interval = 8;
        options.gop.scene_change_detection = false;
        assert((key_frame_indices(encode_video(clip, options).record) == std::vector<int>{0, 8, 16}));

        options.gop.b_frames = 2;
        const StageRecord record = encode_video(clip, options).record;
        assert(std::any_of(record.frames.begin(), record.frames.end(),
                           [](const FrameStats& stats) { return stats.picture_type == PictureType::B; }));

        options.gop = GopStructure::intra_only();
        const StageRecord intra = encode_video(clip, options).record;
        assert(key_frame_indices(intra).size() == clip.size());
    }

    void test_a_constant_qp_holds_for_p_frames()
    {
        const std::vector<Frame> clip = moving_clip(6);
        const StageRecord record =
            encode_video(clip, video_options(VideoCodec::H264, RateControl::constant_qp(30))).record;
        for (const FrameStats& stats : record.frames)
        {
            if (stats.picture_type == PictureType::P)
            {
                assert(stats.qp_mean == 30.0);
            }
        }
        expect_throw<ConfigError>(
            [&] { (void)encode_video(clip, video_options(VideoCodec::H264, RateControl::constant_qp(60))); });
        expect_throw<ConfigError>(
            [&] { (void)encode_video(clip, video_options(VideoCodec::H264, RateControl::quality(50))); });
    }

    void test_a_named_container_is_muxed()
    {
        const std::vector<Frame> clip = moving_clip(6);
        EncodeVideoOptions options = video_options(VideoCodec::H264, RateControl::crf(23));
        options.container = "mp4";
        const EncodedResult encoded = encode_video(clip, options);
        assert(starts_with(encoded.bytes, 4, "ftyp"));
        assert(std::get<EncodeVideoEvidence>(encoded.record.evidence).resolved.global_header);
        assert(probe(Source::from_memory(encoded.bytes)).streams.front().codec_name == "h264");
        check_roundtrip(clip, options, 30.0);

        options.container = "definitely-not-a-muxer";
        expect_throw<UnsupportedCapability>([&] { (void)encode_video(clip, options); });
    }

    void test_hevc_vp9_and_av1_encode_and_decode_back()
    {
        const std::vector<Frame> clip = moving_clip(6);
        if (capabilities().supports(VideoCodec::Hevc))
        {
            const EncodeVideoOptions options = video_options(VideoCodec::Hevc, RateControl::crf(28));
            assert(encode_video(clip, options).record.block_grid->kind == BlockGridKind::Ctu64);
            check_roundtrip(clip, options, 28.0);
        }
        if (capabilities().supports(VideoCodec::Vp9))
        {
            const EncodeVideoOptions options = video_options(VideoCodec::Vp9, RateControl::crf(30));
            const EncodedResult encoded = encode_video(clip, options);
            assert(starts_with(encoded.bytes, 0, "DKIF"));
            check_roundtrip(clip, options, 28.0);
        }
        if (capabilities().supports(VideoCodec::Av1))
        {
            EncodeVideoOptions options = video_options(VideoCodec::Av1, RateControl::crf(35));
            if (capabilities().select_encoder(VideoCodec::Av1, EncoderBackend::Software)->name == "libsvtav1")
            {
                // SVT-AV1 cannot honor keyframes at scene changes.
                expect_throw<ConfigError>([&] { (void)encode_video(clip, options); });
                options.gop.scene_change_detection = false;
            }
            check_roundtrip(clip, options, 26.0);
        }
    }

    void test_encoders_without_b_frames_refuse_them()
    {
        const std::vector<Frame> clip = moving_clip(4);
        for (const VideoCodec codec : {VideoCodec::Vp9, VideoCodec::Av1})
        {
            if (!capabilities().supports(codec))
            {
                continue;
            }
            EncodeVideoOptions options = video_options(codec, RateControl::crf(30));
            options.gop.scene_change_detection = false;
            options.gop.b_frames = 2;
            expect_throw<ConfigError>([&] { (void)encode_video(clip, options); });
        }
    }

    void test_a_bitrate_target_is_passed_through()
    {
        const std::vector<Frame> clip = moving_clip(10);
        const RateControl constrained = RateControl::constrained(200000, 300000, 400000);
        const StageRecord record = encode_video(clip, video_options(VideoCodec::H264, constrained)).record;
        const EncoderResolution& resolved = std::get<EncodeVideoEvidence>(record.evidence).resolved;
        assert(resolved.bit_rate == 200000);
        assert(resolved.max_rate == 300000);
        assert(resolved.buffer_size == 400000);
    }
}

int main()
{
    if (!capabilities().supports(VideoCodec::H264))
    {
        return 77;
    }
    test_h264_encodes_an_annex_b_stream_with_per_frame_statistics();
    test_the_gop_structure_places_keyframes_and_b_frames();
    test_a_constant_qp_holds_for_p_frames();
    test_a_named_container_is_muxed();
    test_hevc_vp9_and_av1_encode_and_decode_back();
    test_encoders_without_b_frames_refuse_them();
    test_a_bitrate_target_is_passed_through();
    return 0;
}
