#include "lossylab/core/error.hpp"
#include "lossylab/core/record.hpp"
#include "lossylab/core/reflect.hpp"
#include "lossylab/env/build_info.hpp"

#include <cassert>
#include <cmath>
#include <utility>

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

    /// A stage that scales from one size to another, with the transform that
    /// goes with it. Resize has no evidence type until resize() is
    /// implemented, so it is filed as a convert, whose kind does not matter
    /// to the transforms.
    StageRecord scaling_stage(const int in_w, const int in_h, const int out_w, const int out_h)
    {
        StageRecord stage;
        stage.evidence = ConvertEvidence{"chroma_up"};
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
        stage.evidence = EncodeVideoEvidence{"", "h264", 1, ColorSpec::bt709_limited(), std::nullopt};
        stage.implementation = "libx264";
        stage.input = format(width, height, "yuv420p", ColorSpec::bt709_limited());
        stage.output = stage.input;
        stage.transform = CoordinateTransform::identity();
        stage.block_grid = BlockGrid::for_kind(grid_kind);
        stage.achieved_bpp = 0.25;
        return stage;
    }

    /// The default configuration of the stages above.
    StageConfiguration configuration_for(const StageRecord& stage)
    {
        switch (stage.kind())
        {
        case StageKind::EncodeVideo: return EncodeVideoOptions{};
        case StageKind::Convert: return ConvertOptions{};
        default: return DecodeImageOptions{};
        }
    }

    void append(ProcessingRecord& record, StageRecord stage)
    {
        StageConfiguration configuration = configuration_for(stage);
        record.append(std::move(stage), std::move(configuration));
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
        ProcessingRecord record;
        append(record, scaling_stage(1920, 1080, 960, 540));
        append(record, scaling_stage(960, 540, 480, 270));

        // Two halvings compose into a quarter, and the composed transform must
        // equal the single-step one, or mask tracing would drift stage by stage.
        assert(record.end_to_end_transform() ==
               CoordinateTransform::resize(1920, 1080, 480, 270));
    }

    void test_an_output_crop_traces_back_to_the_source()
    {
        ProcessingRecord record;
        append(record, scaling_stage(800, 800, 400, 400));

        const Point source = record.end_to_end_transform().map_inverse({0.0, 0.0});
        assert(std::abs(source.x - 0.5) < 1e-9);
        assert(std::abs(source.y - 0.5) < 1e-9);
    }

    void test_an_encode_establishes_a_block_grid()
    {
        ProcessingRecord record;
        append(record, encode_stage(640, 480, BlockGridKind::Macroblock16));

        const std::optional<BlockGrid> grid = record.effective_block_grid();
        assert(grid.has_value());
        assert(grid->valid);
        assert(grid->block_width == 16);
    }

    void test_a_later_resize_destroys_the_grid()
    {
        // The requirement stated in the design, expressed end to end.
        ProcessingRecord record;
        append(record, encode_stage(640, 480, BlockGridKind::Macroblock16));
        append(record, scaling_stage(640, 480, 320, 240));

        const std::optional<BlockGrid> grid = record.effective_block_grid();
        assert(grid.has_value());
        assert(!grid->valid);
    }

    void test_re_encoding_after_a_resize_establishes_a_fresh_grid()
    {
        // What survives a multi-generation chain is the newest grid, not the
        // oldest: the second encode quantizes on its own block boundaries.
        ProcessingRecord record;
        append(record, encode_stage(640, 480, BlockGridKind::Macroblock16));
        append(record, scaling_stage(640, 480, 320, 240));
        append(record, encode_stage(320, 240, BlockGridKind::Ctu64));

        const std::optional<BlockGrid> grid = record.effective_block_grid();
        assert(grid.has_value());
        assert(grid->valid);
        assert(grid->block_width == 64);
    }

    void test_compression_generations_are_counted()
    {
        ProcessingRecord record;
        append(record, encode_stage(640, 480, BlockGridKind::Macroblock16));
        append(record, scaling_stage(640, 480, 320, 240));
        append(record, encode_stage(320, 240, BlockGridKind::Dct8));

        assert(record.compression_generations() == 2);
    }

    void test_one_hardware_stage_makes_the_whole_record_irreproducible()
    {
        ProcessingRecord record;
        append(record, scaling_stage(64, 64, 32, 32));
        assert(record.is_reproducible());

        StageRecord hardware = encode_stage(32, 32, BlockGridKind::Ctu32);
        hardware.implementation = "hevc_nvenc";
        hardware.reproducible = false;
        append(record, hardware);

        assert(!record.is_reproducible());
    }

    void test_conversions_are_collected_across_stages()
    {
        ProcessingRecord record;

        StageRecord first = scaling_stage(64, 64, 32, 32);
        first.conversions.push_back(
            ConversionEvent{"pix_fmt", "rgb24", "yuv420p", ConversionCause::Requested, "swscale"});
        append(record, first);

        StageRecord second = encode_stage(32, 32, BlockGridKind::Dct8);
        second.conversions.push_back(ConversionEvent{"color_range", "full", "limited",
                                                     ConversionCause::CodecConstraint, "libx264"});
        append(record, second);

        const ConversionList all = record.all_conversions();
        assert(all.size() == std::size_t{2});
        assert(all[0].property == std::string("pix_fmt"));
        assert(all[1].property == std::string("color_range"));
    }

    void test_continuity_catches_a_conversion_outside_the_record()
    {
        ProcessingRecord record;
        append(record, scaling_stage(64, 64, 32, 32));
        record.validate_continuity();

        // A second stage that claims a different input than the first produced:
        // something converted the frame without recording it.
        StageRecord mismatched = scaling_stage(32, 32, 16, 16);
        mismatched.input.color = ColorSpec::bt601_limited();
        append(record, mismatched);

        try { record.validate_continuity(); assert(false && "expected throw"); } catch (const ConfigError&) {}
    }

    void test_continuity_catches_a_size_mismatch()
    {
        ProcessingRecord record;
        append(record, scaling_stage(64, 64, 32, 32));
        append(record, scaling_stage(64, 64, 16, 16));  // should have started at 32x32

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
        ProcessingRecord record = ProcessingRecord::for_this_build();
        assert(record.build().at("identity_hash") == build_info().identity_hash);
        assert(record.diagnostics().at("architecture") == diagnostics().architecture);

        StageRecord stage = encode_stage(1920, 1080, BlockGridKind::Ctu64);
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

        EncodeVideoOptions encode_options;
        encode_options.rate_control = RateControl::crf(23.0);
        encode_options.encoder_options = {{"preset", "medium"}};
        record.append(stage, encode_options);
        append(record, scaling_stage(1920, 1080, 960, 540));

        const ProcessingRecord parsed = ProcessingRecord::from_json(record.to_json());

        assert(parsed.build() == record.build());
        assert(parsed.diagnostics() == record.diagnostics());
        assert(parsed.size() == record.size());
        assert(parsed.stages()[0].kind() == StageKind::EncodeVideo);
        assert(std::get<EncodeVideoOptions>(parsed.configurations()[0]).encoder_options.at("preset") == "medium");
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
            StageKind::DecodeImage, StageKind::DecodeVideo, StageKind::Convert, StageKind::ChromaRoundtrip,
            StageKind::Reinterpret, StageKind::Resize, StageKind::Filter, StageKind::EncodeImage,
            StageKind::EncodeVideo, StageKind::RoundtripImage, StageKind::RoundtripVideo, StageKind::AnimateStill,
            StageKind::Measure, StageKind::Compare, StageKind::RecompressionCurve, StageKind::CompressionHistory,
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
        append(chain, first);
        append(chain, second);
        try
        {
            chain.validate_continuity();
            assert(false && "expected throw");
        }
        catch (const ConfigError&)
        {
        }
    }

    void test_configuration_is_kept_once_per_stage_beside_the_stages()
    {
        StageRecord analysis = scaling_stage(64, 64, 64, 64);
        MeasureEvidence measured;
        measured.measured_as = {{"blockiness", "gray"}};
        analysis.evidence = measured;
        analysis.modifies_state = false;

        ConvertOptions convert_options;
        convert_options.chroma_up = KernelSpec{Kernel::Lanczos, {}};
        MeasureOptions measure_options;
        measure_options.analyzers = {Analyzer::Blockiness};

        ProcessingRecord record;
        record.append(scaling_stage(64, 64, 64, 64), convert_options);
        record.append(analysis, measure_options);

        assert(record.configurations().size() == record.stages().size());
        assert(std::get<ConvertOptions>(record.configurations()[0]).chroma_up.kernel == Kernel::Lanczos);
        assert(std::get<MeasureOptions>(record.configurations()[1]).strict == Strict::Refuse);
        assert(record.stages()[0].modifies_state);
        assert(!record.stages()[1].modifies_state);

        const json::Value document = record.to_json();
        assert(document.at("configurations").size() == 2);
        assert(document.at("configurations").at(1).at("analyzers").at(0) == "blockiness");
        assert(document.at("stages").at(1).at("kind") == "measure");
        assert(document.at("stages").at(1).at("evidence").at("measured_as").at("blockiness") == "gray");
        assert(!document.at("stages").at(1).contains("configuration"));
        assert(!document.at("stages").at(1).contains("params"));
        assert(document.at("stages").at(1).at("modifies_state") == false);

        const ProcessingRecord parsed = ProcessingRecord::from_json(document);
        assert(std::get<MeasureOptions>(parsed.configurations()[1]).analyzers ==
               std::vector<Analyzer>{Analyzer::Blockiness});
        assert(std::get<MeasureEvidence>(parsed.stages()[1].evidence).measured_as.at("blockiness") == "gray");
        assert(!parsed.stages()[1].modifies_state);
        assert(parsed.to_json().dump() == document.dump());
    }

    void test_a_configuration_of_another_kind_is_refused()
    {
        ProcessingRecord record;
        try
        {
            record.append(scaling_stage(64, 64, 32, 32), MeasureOptions{});
            assert(false && "expected throw");
        }
        catch (const ConfigError&)
        {
        }
        assert(record.empty());
    }

    void test_a_record_with_mismatched_configurations_is_refused()
    {
        ProcessingRecord record;
        append(record, scaling_stage(64, 64, 32, 32));
        json::Value document = record.to_json();
        document["configurations"] = json::Value::array();
        try
        {
            static_cast<void>(ProcessingRecord::from_json(document));
            assert(false && "expected throw");
        }
        catch (const ConfigError&)
        {
        }
    }

    template <std::size_t... Index>
    void round_trip_every_alternative(std::index_sequence<Index...>)
    {
        const auto round_trip = [](const auto& evidence, const auto& configuration)
        {
            const StageKind kind = stage_kind(StageEvidence{evidence});
            assert(stage_kind(StageConfiguration{configuration}) == kind);
            const json::Value evidence_json = evidence_to_json(evidence);
            const json::Value configuration_json = configuration_to_json(configuration);
            assert(evidence_to_json(evidence_from_json(kind, evidence_json)) == evidence_json);
            assert(configuration_to_json(configuration_from_json(kind, configuration_json)) == configuration_json);
        };
        (round_trip(std::variant_alternative_t<Index, StageEvidence>{},
                    std::variant_alternative_t<Index, StageConfiguration>{}),
         ...);
    }

    void test_every_evidence_and_configuration_round_trips_through_json()
    {
        round_trip_every_alternative(std::make_index_sequence<std::variant_size_v<StageEvidence>>{});
        try
        {
            static_cast<void>(evidence_from_json(StageKind::Resize, json::Value::object()));
            assert(false && "expected throw");
        }
        catch (const ConfigError&)
        {
        }
    }

    void test_a_nan_in_the_evidence_reads_back_as_nan()
    {
        CompareEvidence compared;
        compared.pooled = {{"psnr_std", std::nan("")}};
        const StageEvidence parsed = evidence_from_json(StageKind::Compare, evidence_to_json(compared));
        assert(std::isnan(std::get<CompareEvidence>(parsed).pooled.at("psnr_std")));
    }
}

int main()
{
    test_configuration_is_kept_once_per_stage_beside_the_stages();
    test_a_configuration_of_another_kind_is_refused();
    test_a_record_with_mismatched_configurations_is_refused();
    test_every_evidence_and_configuration_round_trips_through_json();
    test_a_nan_in_the_evidence_reads_back_as_nan();
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
