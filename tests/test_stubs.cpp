#include "lossylab/lossylab.hpp"

#include "lossylab/codec/encode.hpp"
#include "lossylab/core/error.hpp"
#include "lossylab/env/capabilities.hpp"
#include "lossylab/filter/filter_graph.hpp"
#include "lossylab/io/read_headers.hpp"
#include "lossylab/io/video_reader.hpp"
#include "lossylab/measure/measure.hpp"
#include "lossylab/motion/animate_still.hpp"
#include "lossylab/pipeline/pipeline.hpp"
#include "lossylab/resample/resize.hpp"

#include <cassert>
#include <cmath>
#include <cstdlib>
#include <string>
#include <string_view>

using namespace lossylab;

/// The operations below are declared with their final signatures but are not
/// implemented yet. These tests pin down two things that matter now: that
/// argument validation and capability gating happen *before* the unimplemented
/// body, so a caller gets a precise error rather than NotImplemented for a
/// request that was wrong anyway; and that the pure logic living in those files
/// is correct already.

namespace
{
    Frame test_frame(const int width = 64, const int height = 48)
    {
        return Frame::allocate(width, height, PixelFormat::from_name("yuv420p"),
                               ColorSpec::bt709_limited());
    }

    /// Runs `fn` and reports which of the two outcomes it produced.
    template <typename Fn>
    bool throws_not_implemented(Fn&& fn)
    {
        try
        {
            std::forward<Fn>(fn)();
        }
        catch (const NotImplemented&)
        {
            return true;
        }
        catch (...)
        {
            return false;
        }
        return false;
    }

    /// Absolute path to a file under tests/data. Resolved against a
    /// compiled-in directory so tests run from any working directory.
    std::string data_path(const std::string_view name)
    {
        return std::string(LOSSYLAB_TEST_DATA_DIR) + "/" + std::string(name);
    }

    void test_unimplemented_operations_say_which_symbol_is_missing()
    {
        // A skeleton should report precisely what is not written yet, so that a
        // test run over it is an inventory rather than a mystery.
        try
        {
            static_cast<void>(resize(test_frame(), 32, 24));
            assert(false && "expected NotImplemented");
        }
        catch (const NotImplemented& e)
        {
            assert(!e.symbol().empty());
            assert(std::string(e.what()).find(e.symbol()) != std::string::npos);
        }
    }

    // -----------------------------------------------------------------------
    // resize: TargetSize is real
    // -----------------------------------------------------------------------

    void test_target_sizes_resolve_correctly()
    {
        assert(TargetSize::absolute(320, 240).resolve(640, 480) == std::make_pair(320, 240));
        assert(TargetSize::scale(0.5).resolve(640, 480) == std::make_pair(320, 240));
        assert(TargetSize::scale(2.0, 1.0).resolve(64, 48) == std::make_pair(128, 48));

        // The aspect ratio survives a longest-side fit.
        assert(TargetSize::longest_side(100).resolve(400, 200) == std::make_pair(100, 50));
        assert(TargetSize::longest_side(100).resolve(200, 400) == std::make_pair(50, 100));
    }

    void test_an_extreme_downscale_floors_at_one_pixel()
    {
        // Rounding to zero would produce an unusable frame, so one pixel is the
        // floor rather than an error.
        assert(TargetSize::scale(0.001).resolve(64, 48) == std::make_pair(1, 1));
    }

    void test_target_sizes_round_trip_through_json()
    {
        const TargetSize sizes[] = {
            TargetSize::absolute(320, 240),
            TargetSize::scale(0.5, 0.25),
            TargetSize::longest_side(512),
        };
        for (const TargetSize& size : sizes)
        {
            assert(TargetSize::from_json(size.to_json()).resolve(640, 480) ==
                   size.resolve(640, 480));
        }
    }

    void test_resize_rejects_an_empty_frame()
    {
        // A bad request must be reported as such, not as "not implemented".
        try
        {
            (void)(resize(Frame(), 32, 24));
            assert(false && "expected throw");
        }
        catch (const ConfigError&)
        {
        }
    }

    void test_resize_rejects_an_empty_mask()
    {
        ResizeOptions empty_mask;
        empty_mask.size = TargetSize::absolute(32, 24);
        empty_mask.mask = Frame();
        try
        {
            (void)(resize(test_frame(), empty_mask));
            assert(false && "expected throw");
        }
        catch (const ConfigError&)
        {
        }
    }

    void test_a_valid_resize_reaches_the_unwritten_body()
    {
        assert(throws_not_implemented([] { static_cast<void>(resize(test_frame(), 32, 24)); }));
    }

    // -----------------------------------------------------------------------
    // encode: rate control and GOP structure are real
    // -----------------------------------------------------------------------

    void test_rate_control_modes_expose_a_searchable_parameter_only_where_one_exists()
    {
        assert(RateControl::crf(23.0).quality_parameter().has_value());
        assert(RateControl::constant_qp(30).quality_parameter().has_value());
        assert(RateControl::quality(75.0).quality_parameter().has_value());

        // Under a bitrate target, quality is an outcome rather than an input, so
        // there is no parameter for encode_to_target to move.
        assert(!RateControl::bitrate(1000000).quality_parameter().has_value());
        assert(!RateControl::constrained(1000000, 2000000, 4000000)
                    .quality_parameter()
                    .has_value());
    }

    void test_setting_a_quality_parameter_requires_a_mode_that_has_one()
    {
        const RateControl crf = RateControl::crf(23.0);
        assert(std::abs(*crf.with_quality_parameter(30.0).quality_parameter() - 30.0) < 1e-12);

        try
        {
            (void)(RateControl::bitrate(1000000).with_quality_parameter(30.0));
            assert(false && "expected throw");
        }
        catch (const ConfigError&)
        {
        }
    }

    void test_rate_control_rejects_incoherent_settings()
    {
        try
        {
            (void)(RateControl::bitrate(0));
            assert(false && "expected throw");
        }
        catch (const ConfigError&)
        {
        }
        try
        {
            (void)(RateControl::bitrate(-1));
            assert(false && "expected throw");
        }
        catch (const ConfigError&)
        {
        }

        // A max rate below the average rate is not a constraint, it is a mistake.
        try
        {
            (void)(RateControl::constrained(2000000, 1000000, 4000000));
            assert(false && "expected throw");
        }
        catch (const ConfigError&)
        {
        }
        try
        {
            (void)(RateControl::constrained(1000000, 2000000, 0));
            assert(false && "expected throw");
        }
        catch (const ConfigError&)
        {
        }
    }

    void test_rate_control_round_trips_through_json()
    {
        const RateControl controls[] = {
            RateControl::crf(23.0),
            RateControl::constant_qp(30),
            RateControl::bitrate(1500000),
            RateControl::constrained(1500000, 3000000, 6000000),
            RateControl::quality(80.0),
        };
        for (const RateControl& control : controls)
        {
            assert(RateControl::from_json(control.to_json()).describe() == control.describe());
        }
    }

    void test_gop_structures_round_trip_and_intra_only_is_intra_only()
    {
        const GopStructure intra = GopStructure::intra_only();
        assert(intra.keyframe_interval == 1);
        assert(intra.b_frames == 0);
        assert(intra.closed_gop);

        const GopStructure parsed = GopStructure::from_json(intra.to_json());
        assert(parsed.keyframe_interval == intra.keyframe_interval);
        assert(parsed.b_frames == intra.b_frames);
    }

    void test_encode_targets_round_trip()
    {
        EncodeTarget target;
        target.kind = EncodeTarget::Kind::Vmaf;
        target.value = 93.0;
        target.tolerance = 0.5;
        target.max_iterations = 6;

        const EncodeTarget parsed = EncodeTarget::from_json(target.to_json());
        assert(parsed.kind == target.kind);
        assert(std::abs(parsed.value - target.value) < 1e-12);
        assert(parsed.max_iterations == 6);
    }

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

    EncodeVideoOptions h264_encode_options()
    {
        if (!capabilities().supports(VideoCodec::H264))
        {
            // TODO: exit(77) also skips every later test in this file; skip only the calling test.
            std::exit(77);  // no H.264 encoder in this build
        }
        EncodeVideoOptions options;
        options.codec = VideoCodec::H264;
        options.pixel_format = PixelFormat::from_name("yuv420p");
        return options;
    }

    void test_encode_video_rejects_an_empty_frame_sequence()
    {
        try
        {
            (void)(encode_video({}, h264_encode_options()));
            assert(false && "expected throw");
        }
        catch (const ConfigError&)
        {
        }
    }

    void test_encode_video_rejects_an_empty_frame()
    {
        try
        {
            (void)(encode_video({Frame()}, h264_encode_options()));
            assert(false && "expected throw");
        }
        catch (const ConfigError&)
        {
        }
    }

    void test_encode_video_rejects_a_mixed_size_sequence()
    {
        // A mixed-format sequence would have to be converted somewhere, and that
        // conversion has to be its own stage rather than a side effect of encoding.
        const std::vector<Frame> mixed = {test_frame(64, 48), test_frame(32, 24)};
        try
        {
            (void)(encode_video(mixed, h264_encode_options()));
            assert(false && "expected throw");
        }
        catch (const ConfigError&)
        {
        }
    }

    void test_encode_video_rejects_a_pixel_format_the_encoder_does_not_accept()
    {
        EncodeVideoOptions wrong_format = h264_encode_options();
        wrong_format.pixel_format = PixelFormat::from_name("rgb48le");
        try
        {
            (void)(encode_video({test_frame()}, wrong_format));
            assert(false && "expected throw");
        }
        catch (const ConfigError&)
        {
        }
    }

    void test_encode_video_rejects_an_option_the_encoder_does_not_have()
    {
        EncodeVideoOptions bad_option = h264_encode_options();
        bad_option.encoder_options = {{"definitely-not-an-option", "1"}};
        try
        {
            (void)(encode_video({test_frame()}, bad_option));
            assert(false && "expected throw");
        }
        catch (const ConfigError&)
        {
        }
    }

    void test_a_valid_encode_video_request_reaches_the_unwritten_body()
    {
        // TODO: expects encode_video() to be a stub; turn into a real encode test once it is implemented.
        const EncodeVideoOptions options = h264_encode_options();
        assert(throws_not_implemented(
            [&] { static_cast<void>(encode_video({test_frame()}, options)); }));
    }

    void test_encoding_refuses_a_codec_this_build_lacks_before_anything_else()
    {
        const AbsentEncoder absent = absent_video_encoder();
        EncodeVideoOptions options;
        options.codec = absent.codec;
        options.backend = absent.backend;
        options.pixel_format = PixelFormat::from_name("yuv420p");

        try
        {
            (void)(encode_video({test_frame()}, options));
            assert(false && "expected throw");
        }
        catch (const UnsupportedCapability&)
        {
        }
    }

    void test_encode_to_target_needs_a_parameter_it_can_search()
    {
        if (!capabilities().supports(VideoCodec::H264))
        {
            // TODO: exit(77) also skips every later test in this file; skip only this test.
            std::exit(77);  // no H.264 encoder in this build
        }

        EncodeVideoOptions options;
        options.codec = VideoCodec::H264;
        options.pixel_format = PixelFormat::from_name("yuv420p");
        options.rate_control = RateControl::bitrate(1000000);

        EncodeTarget target;
        target.kind = EncodeTarget::Kind::BitsPerPixel;

        // Searching a bitrate-targeted encode for a quality parameter is
        // incoherent, and saying so beats failing obscurely later.
        try
        {
            (void)(encode_to_target({test_frame()}, options, target));
            assert(false && "expected throw");
        }
        catch (const ConfigError&)
        {
        }

        options.rate_control = RateControl::crf(23.0);
        EncodeTarget bad_tolerance = target;
        bad_tolerance.tolerance = 0.0;
        try
        {
            (void)(encode_to_target({test_frame()}, options, bad_tolerance));
            assert(false && "expected throw");
        }
        catch (const ConfigError&)
        {
        }

        assert(throws_not_implemented(
            [&] { static_cast<void>(encode_to_target({test_frame()}, options, target)); }));
    }

    // -----------------------------------------------------------------------
    // FilterGraph
    // -----------------------------------------------------------------------

    void test_a_filter_graph_validates_its_inputs()
    {
        FilterGraphOptions options;
        try
        {
            FilterGraph graph(options);  // no description
            assert(false && "expected throw");
        }
        catch (const ConfigError&)
        {
        }

        options.description = "[in]null[out]";
        try
        {
            FilterGraph graph(options);  // no inputs
            assert(false && "expected throw");
        }
        catch (const ConfigError&)
        {
        }

        options.inputs.push_back(FilterInput::from_frame(test_frame()));
        options.thread_count = 0;
        try
        {
            FilterGraph graph(options);
            assert(false && "expected throw");
        }
        catch (const ConfigError&)
        {
        }

        options.thread_count = 1;
        assert(throws_not_implemented([&] { FilterGraph graph(options); }));
    }

    void test_a_filter_input_describes_the_frame_it_came_from()
    {
        const Frame frame = test_frame(64, 48);
        const FilterInput input = FilterInput::from_frame(frame, "source");

        assert(input.name == std::string("source"));
        assert(input.width == 64);
        assert(input.height == 48);
        assert(input.pixel_format == frame.pixel_format());
        assert(input.color == frame.color());

        try
        {
            (void)(FilterInput::from_frame(Frame()));
            assert(false && "expected throw");
        }
        catch (const ConfigError&)
        {
        }
    }

    void test_a_filter_input_must_carry_a_fully_specified_color()
    {
        FilterGraphOptions options;
        options.description = "[in]null[out]";

        FilterInput input = FilterInput::from_frame(test_frame());
        input.color = ColorSpec();
        options.inputs.push_back(input);

        try
        {
            FilterGraph graph(options);
            assert(false && "expected throw");
        }
        catch (const ConfigError&)
        {
        }
    }

    // -----------------------------------------------------------------------
    // animate_still: the trajectory is real
    // -----------------------------------------------------------------------

    void test_a_static_trajectory_is_the_identity_at_every_frame()
    {
        const Trajectory trajectory;
        for (int i = 0; i < 25; ++i)
        {
            assert(trajectory.at(i, 25, Rational{25, 1}).is_identity());
        }
    }

    /// 26 frames at 25 fps: the last frame sits at index 25, i.e. 25/25 = exactly
    /// one second, which keeps the arithmetic below readable. Note that a clip's
    /// last frame is at (count - 1) / fps, not count / fps.
    constexpr int one_second_frames = 26;
    constexpr int last_frame = one_second_frames - 1;
    constexpr Rational fps_25{25, 1};

    void test_a_pan_moves_by_the_right_amount()
    {
        Trajectory trajectory;
        trajectory.pan_x = 10.0;  // pixels per second

        // After one second the pan totals 10 pixels, and the content has moved left
        // in frame coordinates because the camera moved right.
        const CoordinateTransform last = trajectory.at(last_frame, one_second_frames, fps_25);
        assert(std::abs(last.translate_x - -10.0) < 1e-9);

        // The first frame has not moved at all.
        assert(trajectory.at(0, one_second_frames, fps_25).is_identity());

        // Halfway through, halfway there.
        assert(std::abs(trajectory.at(12, one_second_frames, fps_25).translate_x - -4.8) < 1e-9);
    }

    void test_a_zoom_compounds_over_time()
    {
        Trajectory trajectory;
        trajectory.zoom_rate = 2.0;  // doubles every second

        const CoordinateTransform last = trajectory.at(last_frame, one_second_frames, fps_25);
        assert(std::abs(last.scale_x - 2.0) < 1e-9);
        assert(std::abs(last.scale_y - 2.0) < 1e-9);

        // Compounding, not linear: doubling the elapsed time squares the zoom.
        // Frames 5 and 10 sit at 0.2 s and 0.4 s.
        const double at_one = trajectory.at(5, one_second_frames, fps_25).scale_x;
        const double at_two = trajectory.at(10, one_second_frames, fps_25).scale_x;
        assert(std::abs(at_two - at_one * at_one) < 1e-9);
        assert(at_two < 2.0 * at_one);
    }

    void test_easing_bends_the_path_not_the_endpoints()
    {
        Trajectory eased;
        eased.pan_x = 10.0;
        eased.ease = true;

        // The endpoints must still be reached exactly; only the middle differs.
        assert(eased.at(0, one_second_frames, fps_25).is_identity());
        assert(std::abs(eased.at(last_frame, one_second_frames, fps_25).translate_x - -10.0) < 1e-9);

        Trajectory linear = eased;
        linear.ease = false;
        assert(eased.at(10, one_second_frames, fps_25) !=
               linear.at(10, one_second_frames, fps_25));
    }

    void test_a_frame_transform_inverts_back_to_the_still()
    {
        // The property that makes an inpainting mask traceable through an
        // animation: a point in frame i maps back to a point in the original.
        Trajectory trajectory;
        trajectory.pan_x = 20.0;
        trajectory.zoom_rate = 1.5;
        trajectory.rotation_rate = 10.0;

        const CoordinateTransform transform = trajectory.at(15, 25, Rational{25, 1});
        const Point original{100.0, 75.0};
        const Point round_tripped = transform.map_inverse(transform.map_forward(original));

        assert(std::abs(round_tripped.x - original.x) < 1e-6);
        assert(std::abs(round_tripped.y - original.y) < 1e-6);
    }

    void test_a_trajectory_rejects_an_impossible_request()
    {
        const Trajectory trajectory;
        try
        {
            (void)(trajectory.at(0, 0, Rational{25, 1}));
            assert(false && "expected throw");
        }
        catch (const ConfigError&)
        {
        }
        try
        {
            (void)(trajectory.at(25, 25, Rational{25, 1}));
            assert(false && "expected throw");
        }
        catch (const ConfigError&)
        {
        }
        try
        {
            (void)(trajectory.at(-1, 25, Rational{25, 1}));
            assert(false && "expected throw");
        }
        catch (const ConfigError&)
        {
        }
        try
        {
            (void)(trajectory.at(0, 25, Rational{0, 1}));
            assert(false && "expected throw");
        }
        catch (const ConfigError&)
        {
        }
    }

    void test_a_single_frame_animation_sits_at_the_start()
    {
        Trajectory trajectory;
        trajectory.pan_x = 100.0;
        assert(trajectory.at(0, 1, Rational{25, 1}).is_identity());
    }

    void test_trajectories_and_noise_round_trip_through_json()
    {
        Trajectory trajectory;
        trajectory.pan_x = 3.5;
        trajectory.zoom_rate = 1.2;
        trajectory.rotation_rate = -4.0;
        trajectory.ease = true;

        const Trajectory parsed = Trajectory::from_json(trajectory.to_json());
        assert(std::abs(parsed.pan_x - 3.5) < 1e-12);
        assert(std::abs(parsed.zoom_rate - 1.2) < 1e-12);
        assert(parsed.ease);

        TemporalNoise noise;
        noise.sigma = 2.5;
        noise.temporal_correlation = 0.3;
        const TemporalNoise parsed_noise = TemporalNoise::from_json(noise.to_json());
        assert(std::abs(parsed_noise.sigma - 2.5) < 1e-12);
    }

    void test_animate_still_rejects_an_empty_frame()
    {
        try
        {
            (void)(animate_still(Frame(), AnimateStillOptions()));
            assert(false && "expected throw");
        }
        catch (const ConfigError&)
        {
        }
    }

    void test_animate_still_rejects_a_zero_frame_count()
    {
        AnimateStillOptions options;
        options.frame_count = 0;
        try
        {
            (void)(animate_still(test_frame(), options));
            assert(false && "expected throw");
        }
        catch (const ConfigError&)
        {
        }
    }

    void test_animate_still_rejects_a_temporal_correlation_outside_its_range()
    {
        AnimateStillOptions options;
        options.frame_count = 10;
        options.temporal_noise.temporal_correlation = 1.5;
        try
        {
            (void)(animate_still(test_frame(), options));
            assert(false && "expected throw");
        }
        catch (const ConfigError&)
        {
        }
    }

    void test_animate_still_rejects_a_negative_noise_sigma()
    {
        AnimateStillOptions options;
        options.frame_count = 10;
        options.temporal_noise.temporal_correlation = 0.0;
        options.temporal_noise.sigma = -1.0;
        try
        {
            (void)(animate_still(test_frame(), options));
            assert(false && "expected throw");
        }
        catch (const ConfigError&)
        {
        }
    }

    void test_a_valid_animate_still_request_reaches_the_unwritten_body()
    {
        AnimateStillOptions options;
        options.frame_count = 10;
        options.temporal_noise.temporal_correlation = 0.0;
        options.temporal_noise.sigma = 0.0;
        assert(throws_not_implemented(
            [&] { static_cast<void>(animate_still(test_frame(), options)); }));
    }

    // -----------------------------------------------------------------------
    // measure and compare
    // -----------------------------------------------------------------------

    void test_analyzer_names_round_trip()
    {
        const Analyzer analyzers[] = {
            Analyzer::SignalLevels, Analyzer::Blockiness,  Analyzer::Blurriness,
            Analyzer::Letterbox,    Analyzer::Interlacing, Analyzer::SpatialTemporalInfo,
            Analyzer::SceneChange,  Analyzer::DuplicateFrames,
        };
        for (const Analyzer analyzer : analyzers)
        {
            assert(analyzer_from_string(to_string(analyzer)) == analyzer);
        }
        try
        {
            (void)(analyzer_from_string("vibes"));
            assert(false && "expected throw");
        }
        catch (const ConfigError&)
        {
        }
    }

    void test_measure_validates_before_it_gives_up()
    {
        try
        {
            (void)(measure(std::vector<Frame>{}, {Analyzer::Blockiness}));
            assert(false && "expected throw");
        }
        catch (const ConfigError&)
        {
        }
        try
        {
            (void)(measure(test_frame(), {}));
            assert(false && "expected throw");
        }
        catch (const ConfigError&)
        {
        }
    }

    void test_compare_insists_the_two_sides_line_up()
    {
        const Frame a = test_frame(64, 48);
        const Frame b = test_frame(32, 24);

        try
        {
            (void)(compare({a}, {}, {Metric::Psnr}));
            assert(false && "expected throw");
        }
        catch (const ConfigError&)
        {
        }
        try
        {
            (void)(compare({a, a}, {a}, {Metric::Psnr}));
            assert(false && "expected throw");
        }
        catch (const ConfigError&)
        {
        }
        try
        {
            (void)(compare({a}, {a}, {}));
            assert(false && "expected throw");
        }
        catch (const ConfigError&)
        {
        }

        // Comparing frames of different sizes would silently measure a resize
        // rather than the degradation under test.
        try
        {
            (void)(compare({a}, {b}, {Metric::Psnr}));
            assert(false && "expected throw");
        }
        catch (const ConfigError&)
        {
        }
    }

    void test_vmaf_is_gated_when_the_build_lacks_it()
    {
        const Frame frame = test_frame();
        if (capabilities().supports(Metric::Vmaf))
        {
            // TODO: expects compare() to be a stub; assert on the VMAF result once it is implemented.
            assert(throws_not_implemented([&] { static_cast<void>(compare({frame}, {frame}, {Metric::Vmaf})); }));
            return;
        }
        try
        {
            (void)(compare({frame}, {frame}, {Metric::Vmaf}));
            assert(false && "expected throw");
        }
        catch (const UnsupportedCapability&)
        {
        }
    }

    void test_a_recompression_sweep_needs_enough_points_to_find_a_minimum()
    {
        RecompressionOptions options;
        options.codec = ImageCodec::Mjpeg;
        options.pixel_format = PixelFormat::from_name("yuvj420p");
        options.parameter_range = {50.0, 75.0};

        // Two points cannot show a knee, so asking for one is a mistake rather
        // than a result with low confidence.
        try
        {
            (void)(recompression_curve(test_frame(), options));
            assert(false && "expected throw");
        }
        catch (const ConfigError&)
        {
        }
        try
        {
            (void)(recompression_curve(Frame(), options));
            assert(false && "expected throw");
        }
        catch (const ConfigError&)
        {
        }
    }

    // -----------------------------------------------------------------------
    // VideoReader selectors
    // -----------------------------------------------------------------------

    void test_indices_selector_rejects_a_negative_index()
    {
        try
        {
            (void)(FrameSelector::indices({0, -1}));
            assert(false && "expected throw");
        }
        catch (const ConfigError&)
        {
        }
    }

    void test_stride_selector_rejects_a_zero_stride()
    {
        try
        {
            (void)(FrameSelector::stride(0));
            assert(false && "expected throw");
        }
        catch (const ConfigError&)
        {
        }
    }

    void test_stride_selector_rejects_a_negative_offset()
    {
        try
        {
            (void)(FrameSelector::stride(2, -1));
            assert(false && "expected throw");
        }
        catch (const ConfigError&)
        {
        }
    }

    void test_picture_types_selector_rejects_an_empty_set()
    {
        try
        {
            (void)(FrameSelector::picture_types({}));
            assert(false && "expected throw");
        }
        catch (const ConfigError&)
        {
        }
    }

    void test_evenly_spaced_selector_rejects_a_zero_count()
    {
        try
        {
            (void)(FrameSelector::evenly_spaced(0));
            assert(false && "expected throw");
        }
        catch (const ConfigError&)
        {
        }
    }

    void test_where_selector_rejects_a_null_predicate()
    {
        try
        {
            (void)(FrameSelector::where(nullptr));
            assert(false && "expected throw");
        }
        catch (const ConfigError&)
        {
        }
    }

    void test_frame_selectors_serialize_so_a_spec_can_record_them()
    {
        assert(FrameSelector::all().to_json().at("select").get<std::string>() == std::string("all"));
        assert(FrameSelector::stride(5, 2).to_json().at("step").get<std::int64_t>() == 5);
        assert(FrameSelector::evenly_spaced(8).to_json().at("count").get<std::int64_t>() == 8);

        const json::Value types =
            FrameSelector::picture_types({PictureType::I}).to_json();
        assert(types.at("types").size() == std::size_t{1});
    }

    void test_a_predicate_selector_admits_it_cannot_be_replayed()
    {
        // Honesty matters more than coverage here: a closure cannot be serialized,
        // so a spec containing one must not claim to be reproducible.
        const json::Value document =
            FrameSelector::where([](const VideoFrame&) { return true; }).to_json();
        assert(!document.at("replayable").get<bool>());
    }

    void test_selectors_compose()
    {
        const FrameSelector combined =
            FrameSelector::picture_types({PictureType::I}).and_also(FrameSelector::stride(10));

        const json::Value document = combined.to_json();
        assert(document.at("select").get<std::string>() == std::string("and"));
        assert(document.at("left").at("select").get<std::string>() == std::string("picture_types"));
        assert(document.at("right").at("select").get<std::string>() == std::string("stride"));
    }

    void test_a_video_reader_reports_a_missing_stream_rather_than_failing_later()
    {
        VideoReaderOptions options;
        options.thread_count = 0;
        try
        {
            VideoReader reader(Source::from_path(data_path("testsrc_64x48.mp4")), options);
            assert(false && "expected throw");
        }
        catch (const ConfigError&)
        {
        }

        try
        {
            VideoReader reader(Source::from_path("/nonexistent/clip.mp4"));
            assert(false && "expected throw");
        }
        catch (const Error&)
        {
        }
    }

    // -----------------------------------------------------------------------
    // read_headers and Pipeline
    // -----------------------------------------------------------------------

    void test_read_headers_validates_before_it_gives_up()
    {
        ReadHeadersOptions options;
        options.max_slices = -1;
        try
        {
            (void)(read_headers(Source::from_path(data_path("testsrc_64x48.mp4")), options));
            assert(false && "expected throw");
        }
        catch (const ConfigError&)
        {
        }
    }

    void test_a_pipeline_validates_its_spec_at_construction()
    {
        // Before a dataset starts, not on sample forty thousand.
        PipelineSpec bad;
        bad.add(StageKind::Convert, json::object({{"pix_fmt", "yuv420q"}}));
        try
        {
            Pipeline pipeline(bad);
            assert(false && "expected throw");
        }
        catch (const ConfigError&)
        {
        }

        try
        {
            Pipeline pipeline{PipelineSpec()};
            assert(false && "expected throw");
        }
        catch (const ConfigError&)
        {
        }
    }

    void test_a_valid_pipeline_constructs_and_keeps_its_spec()
    {
        PipelineSpec spec;
        spec.add(StageKind::Convert, json::object({{"pix_fmt", "yuv420p"}}));

        Pipeline pipeline(spec);
        assert(pipeline.spec().size() == std::size_t{1});
        assert(pipeline.spec().spec_id() == spec.spec_id());

        try
        {
            (void)(pipeline.run(std::vector<Frame>{}));
            assert(false && "expected throw");
        }
        catch (const ConfigError&)
        {
        }
        try
        {
            (void)(pipeline.run(Frame()));
            assert(false && "expected throw");
        }
        catch (const ConfigError&)
        {
        }
        assert(throws_not_implemented([&] { static_cast<void>(pipeline.run(test_frame())); }));
    }

    // -----------------------------------------------------------------------
    // The umbrella header
    // -----------------------------------------------------------------------

    void test_the_umbrella_header_compiles_and_covers_the_surface()
    {
        // Included below rather than at the top so that this file also proves the
        // individual headers are self-contained.
        assert(!build_info().build_id.empty());
    }
}

int main()
{
    test_unimplemented_operations_say_which_symbol_is_missing();
    test_target_sizes_resolve_correctly();
    test_an_extreme_downscale_floors_at_one_pixel();
    test_target_sizes_round_trip_through_json();
    test_resize_rejects_an_empty_frame();
    test_resize_rejects_an_empty_mask();
    test_a_valid_resize_reaches_the_unwritten_body();
    test_rate_control_modes_expose_a_searchable_parameter_only_where_one_exists();
    test_setting_a_quality_parameter_requires_a_mode_that_has_one();
    test_rate_control_rejects_incoherent_settings();
    test_rate_control_round_trips_through_json();
    test_gop_structures_round_trip_and_intra_only_is_intra_only();
    test_encode_targets_round_trip();
    test_encode_video_rejects_an_empty_frame_sequence();
    test_encode_video_rejects_an_empty_frame();
    test_encode_video_rejects_a_mixed_size_sequence();
    test_encode_video_rejects_a_pixel_format_the_encoder_does_not_accept();
    test_encode_video_rejects_an_option_the_encoder_does_not_have();
    test_a_valid_encode_video_request_reaches_the_unwritten_body();
    test_encoding_refuses_a_codec_this_build_lacks_before_anything_else();
    test_encode_to_target_needs_a_parameter_it_can_search();
    test_a_filter_graph_validates_its_inputs();
    test_a_filter_input_describes_the_frame_it_came_from();
    test_a_filter_input_must_carry_a_fully_specified_color();
    test_a_static_trajectory_is_the_identity_at_every_frame();
    test_a_pan_moves_by_the_right_amount();
    test_a_zoom_compounds_over_time();
    test_easing_bends_the_path_not_the_endpoints();
    test_a_frame_transform_inverts_back_to_the_still();
    test_a_trajectory_rejects_an_impossible_request();
    test_a_single_frame_animation_sits_at_the_start();
    test_trajectories_and_noise_round_trip_through_json();
    test_animate_still_rejects_an_empty_frame();
    test_animate_still_rejects_a_zero_frame_count();
    test_animate_still_rejects_a_temporal_correlation_outside_its_range();
    test_animate_still_rejects_a_negative_noise_sigma();
    test_a_valid_animate_still_request_reaches_the_unwritten_body();
    test_analyzer_names_round_trip();
    test_measure_validates_before_it_gives_up();
    test_compare_insists_the_two_sides_line_up();
    test_vmaf_is_gated_when_the_build_lacks_it();
    test_a_recompression_sweep_needs_enough_points_to_find_a_minimum();
    test_indices_selector_rejects_a_negative_index();
    test_stride_selector_rejects_a_zero_stride();
    test_stride_selector_rejects_a_negative_offset();
    test_picture_types_selector_rejects_an_empty_set();
    test_evenly_spaced_selector_rejects_a_zero_count();
    test_where_selector_rejects_a_null_predicate();
    test_frame_selectors_serialize_so_a_spec_can_record_them();
    test_a_predicate_selector_admits_it_cannot_be_replayed();
    test_selectors_compose();
    test_a_video_reader_reports_a_missing_stream_rather_than_failing_later();
    test_read_headers_validates_before_it_gives_up();
    test_a_pipeline_validates_its_spec_at_construction();
    test_a_valid_pipeline_constructs_and_keeps_its_spec();
    test_the_umbrella_header_compiles_and_covers_the_surface();
}
