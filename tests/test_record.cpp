#include "lossylab/core/error.hpp"
#include "lossylab/core/record.hpp"

#include <cassert>
#include <cmath>

using namespace lossylab;

namespace
{
    FormatDescription format(const int width, const int height, const char* pix_fmt,
                             const ColorSpec& color)
    {
        FormatDescription f;
        f.width = width;
        f.height = height;
        f.pixel_format = PixelFormat::from_name(pix_fmt);
        f.color = color;
        return f;
    }

    /// A resize stage from one size to another, with the transform that goes
    /// with it. Used to build multi-stage records in the tests below.
    StageRecord resize_stage(const int in_w, const int in_h, const int out_w, const int out_h)
    {
        StageRecord stage;
        stage.kind = StageKind::Resize;
        stage.implementation = "swscale";
        stage.input = format(in_w, in_h, "yuv420p", ColorSpec::bt709_limited());
        stage.output = format(out_w, out_h, "yuv420p", ColorSpec::bt709_limited());
        stage.transform = CoordinateTransform::resize(in_w, in_h, out_w, out_h);
        return stage;
    }

    /// An encode stage, which imposes a block grid.
    StageRecord encode_stage(const int width, const int height, const BlockGridKind grid_kind)
    {
        StageRecord stage;
        stage.kind = StageKind::EncodeVideo;
        stage.implementation = "libx264";
        stage.input = format(width, height, "yuv420p", ColorSpec::bt709_limited());
        stage.output = stage.input;
        stage.transform = CoordinateTransform::identity();
        stage.block_grid = BlockGrid::for_kind(grid_kind);
        stage.achieved_bpp = 0.25;
        return stage;
    }

    void test_an_empty_record_is_the_identity()
    {
        const ProcessingRecord record;
        assert(record.empty());
        assert(record.end_to_end_transform().is_identity());
        assert(!record.effective_block_grid().has_value());
        assert(record.is_reproducible());
        assert(record.compression_generations() == 0);
    }

    void test_transforms_compose_across_stages()
    {
        ProcessingRecord record("test-build");
        record.append(resize_stage(1920, 1080, 960, 540));
        record.append(resize_stage(960, 540, 480, 270));

        // Two halvings compose into a quarter, and the composed transform must
        // equal the single-step one, or mask tracing would drift stage by stage.
        assert(record.end_to_end_transform() ==
               CoordinateTransform::resize(1920, 1080, 480, 270));
    }

    void test_an_output_crop_traces_back_to_the_source()
    {
        ProcessingRecord record("test-build");
        record.append(resize_stage(800, 800, 400, 400));

        const Point source = record.end_to_end_transform().map_inverse({0.0, 0.0});
        assert(std::abs(source.x - 0.5) < 1e-9);
        assert(std::abs(source.y - 0.5) < 1e-9);
    }

    void test_an_encode_establishes_a_block_grid()
    {
        ProcessingRecord record("test-build");
        record.append(encode_stage(640, 480, BlockGridKind::Macroblock16));

        const std::optional<BlockGrid> grid = record.effective_block_grid();
        assert(grid.has_value());
        assert(grid->valid);
        assert(grid->block_width == 16);
    }

    void test_a_later_resize_destroys_the_grid()
    {
        // The requirement stated in the design, expressed end to end.
        ProcessingRecord record("test-build");
        record.append(encode_stage(640, 480, BlockGridKind::Macroblock16));
        record.append(resize_stage(640, 480, 320, 240));

        const std::optional<BlockGrid> grid = record.effective_block_grid();
        assert(grid.has_value());
        assert(!grid->valid);
    }

    void test_re_encoding_after_a_resize_establishes_a_fresh_grid()
    {
        // What survives a multi-generation chain is the newest grid, not the
        // oldest: the second encode quantizes on its own block boundaries.
        ProcessingRecord record("test-build");
        record.append(encode_stage(640, 480, BlockGridKind::Macroblock16));
        record.append(resize_stage(640, 480, 320, 240));
        record.append(encode_stage(320, 240, BlockGridKind::Ctu64));

        const std::optional<BlockGrid> grid = record.effective_block_grid();
        assert(grid.has_value());
        assert(grid->valid);
        assert(grid->block_width == 64);
    }

    void test_compression_generations_are_counted()
    {
        ProcessingRecord record("test-build");
        record.append(encode_stage(640, 480, BlockGridKind::Macroblock16));
        record.append(resize_stage(640, 480, 320, 240));
        record.append(encode_stage(320, 240, BlockGridKind::Dct8));

        assert(record.compression_generations() == 2);
    }

    void test_one_hardware_stage_makes_the_whole_record_irreproducible()
    {
        ProcessingRecord record("test-build");
        record.append(resize_stage(64, 64, 32, 32));
        assert(record.is_reproducible());

        StageRecord hardware = encode_stage(32, 32, BlockGridKind::Ctu32);
        hardware.implementation = "hevc_nvenc";
        hardware.reproducible = false;
        record.append(hardware);

        assert(!record.is_reproducible());
    }

    void test_conversions_are_collected_across_stages()
    {
        ProcessingRecord record("test-build");

        StageRecord first = resize_stage(64, 64, 32, 32);
        first.conversions.push_back(
            ConversionEvent{"pix_fmt", "rgb24", "yuv420p", ConversionCause::Requested, "swscale"});
        record.append(first);

        StageRecord second = encode_stage(32, 32, BlockGridKind::Dct8);
        second.conversions.push_back(ConversionEvent{"color_range", "full", "limited",
                                                     ConversionCause::CodecConstraint, "libx264"});
        record.append(second);

        const ConversionList all = record.all_conversions();
        assert(all.size() == std::size_t{2});
        assert(all[0].property == std::string("pix_fmt"));
        assert(all[1].property == std::string("color_range"));
    }

    void test_continuity_catches_a_conversion_outside_the_record()
    {
        ProcessingRecord record("test-build");
        record.append(resize_stage(64, 64, 32, 32));
        record.validate_continuity();

        // A second stage that claims a different input than the first produced:
        // something converted the frame without recording it.
        StageRecord mismatched = resize_stage(32, 32, 16, 16);
        mismatched.input.color = ColorSpec::bt601_limited();
        record.append(mismatched);

        try { record.validate_continuity(); assert(false && "expected throw"); } catch (const ConfigError&) {}
    }

    void test_continuity_catches_a_size_mismatch()
    {
        ProcessingRecord record("test-build");
        record.append(resize_stage(64, 64, 32, 32));
        record.append(resize_stage(64, 64, 16, 16));  // should have started at 32x32

        try { record.validate_continuity(); assert(false && "expected throw"); } catch (const ConfigError&) {}
    }

    void test_frame_stats_round_trip_including_absent_measurements()
    {
        FrameStats stats;
        stats.index = 3;
        stats.pts = 12000;
        stats.picture_type = PictureType::B;
        stats.key_frame = false;
        stats.size_bytes = 4096;
        stats.qp_mean = 27.5;
        // qp_min and qp_max stay absent: a codec that does not expose them must be
        // distinguishable from one that reported zero.

        const FrameStats parsed = FrameStats::from_json(stats.to_json());
        assert(parsed.index == 3);
        assert(parsed.picture_type == PictureType::B);
        assert(parsed.size_bytes.has_value());
        assert(*parsed.size_bytes == std::int64_t{4096});
        assert(parsed.qp_mean.has_value());
        assert(std::abs(*parsed.qp_mean - 27.5) < 1e-12);
        assert(!parsed.qp_min.has_value());
        assert(!parsed.qp_max.has_value());
    }

    void test_a_full_record_round_trips_through_json()
    {
        ProcessingRecord record("ffmpeg-n8.0.1-abc123");

        StageRecord stage = encode_stage(1920, 1080, BlockGridKind::Ctu64);
        stage.params = json::object({{"crf", 23}, {"preset", "medium"}});
        stage.encoder_settings = json::object({{"rate_control", "crf"}, {"gop", 250}});
        stage.seed = std::uint64_t{0xDEADBEEF};
        stage.duration_ms = 12.5;
        stage.ffmpeg_duration_ms = 9.25;
        stage.conversions.push_back(
            ConversionEvent{"pix_fmt", "rgb24", "yuv420p", ConversionCause::CodecConstraint, "libx264"});

        FrameStats frame;
        frame.index = 0;
        frame.picture_type = PictureType::I;
        frame.key_frame = true;
        frame.size_bytes = 50000;
        stage.frames.push_back(frame);

        record.append(stage);
        record.append(resize_stage(1920, 1080, 960, 540));

        const ProcessingRecord parsed = ProcessingRecord::from_json(record.to_json());

        assert(parsed.build_id() == record.build_id());
        assert(parsed.size() == record.size());
        assert(parsed.compression_generations() == record.compression_generations());
        assert(parsed.end_to_end_transform() == record.end_to_end_transform());
        assert(parsed.all_conversions().size() == std::size_t{1});
        assert(parsed.to_json().at("stages").at(0).at("ffmpeg_duration_ms").get<double>() == 9.25);

        // Serializing twice gives byte-identical output, which is what makes
        // records comparable across runs and across classes.
        assert(parsed.to_json().dump() == record.to_json().dump());
    }

    void test_stage_kind_names_round_trip()
    {
        const StageKind kinds[] = {
            StageKind::Decode, StageKind::Probe, StageKind::Convert, StageKind::ChromaRoundtrip,
            StageKind::Reinterpret, StageKind::Resize, StageKind::Filter, StageKind::EncodeImage,
            StageKind::EncodeVideo, StageKind::Roundtrip, StageKind::AnimateStill,
            StageKind::Measure, StageKind::Compare, StageKind::RecompressionCurve,
        };
        for (const StageKind kind : kinds)
        {
            assert(stage_kind_from_string(to_string(kind)) == kind);
        }
        try { stage_kind_from_string("transcode"); assert(false && "expected throw"); } catch (const ConfigError&) {}
    }

    void test_picture_type_names_round_trip()
    {
        for (const PictureType type :
             {PictureType::Unknown, PictureType::I, PictureType::P, PictureType::B})
        {
            assert(picture_type_from_string(to_string(type)) == type);
        }
        try { picture_type_from_string("S"); assert(false && "expected throw"); } catch (const ConfigError&) {}
    }

    void test_embedded_data_is_part_of_the_format()
    {
        FormatDescription described = format(64, 48, "rgb24", ColorSpec::srgb());
        described.icc_profile = "Display P3";
        described.orientation = 6;
        described.sample_aspect_ratio = Rational{4, 3};
        assert(FormatDescription::from_json(described.to_json()) == described);
        assert(described.to_json().contains("pixel_format"));

        // A profile, orientation or pixel shape lost between two stages breaks
        // the chain like any other format change.
        FormatDescription without_profile = described;
        without_profile.icc_profile.clear();
        assert(without_profile != described);
        StageRecord first;
        first.output = described;
        StageRecord second;
        second.input = without_profile;
        second.output = without_profile;
        ProcessingRecord chain;
        chain.append(first);
        chain.append(second);
        try
        {
            chain.validate_continuity();
            assert(false && "expected throw");
        }
        catch (const ConfigError&)
        {
        }
    }
}

int main()
{
    test_an_empty_record_is_the_identity();
    test_transforms_compose_across_stages();
    test_an_output_crop_traces_back_to_the_source();
    test_an_encode_establishes_a_block_grid();
    test_a_later_resize_destroys_the_grid();
    test_re_encoding_after_a_resize_establishes_a_fresh_grid();
    test_compression_generations_are_counted();
    test_one_hardware_stage_makes_the_whole_record_irreproducible();
    test_conversions_are_collected_across_stages();
    test_continuity_catches_a_conversion_outside_the_record();
    test_continuity_catches_a_size_mismatch();
    test_frame_stats_round_trip_including_absent_measurements();
    test_a_full_record_round_trips_through_json();
    test_stage_kind_names_round_trip();
    test_picture_type_names_round_trip();
    test_embedded_data_is_part_of_the_format();
}
