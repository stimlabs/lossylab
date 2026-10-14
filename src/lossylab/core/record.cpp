#include "lossylab/core/record.hpp"

#include "lossylab/core/error.hpp"
#include "lossylab/core/json_io.hpp"
#include "lossylab/core/schema_version.hpp"
#include "lossylab/env/build_info.hpp"

#include <algorithm>
#include <array>
#include <string_view>
#include <utility>

namespace lossylab
{
    std::string to_string(const StageKind kind)
    {
        switch (kind)
        {
        case StageKind::DecodeImage: return "decode_image";
        case StageKind::DecodeVideo: return "decode_video";
        case StageKind::Convert: return "convert";
        case StageKind::ChromaRoundtrip: return "chroma_roundtrip";
        case StageKind::Reinterpret: return "reinterpret";
        case StageKind::Crop: return "crop";
        case StageKind::Orient: return "orient";
        case StageKind::Achromatic: return "achromatic";
        case StageKind::Resize: return "resize";
        case StageKind::Filter: return "filter";
        case StageKind::EncodeImage: return "encode_image";
        case StageKind::EncodeVideo: return "encode_video";
        case StageKind::RoundtripImage: return "roundtrip_image";
        case StageKind::RoundtripVideo: return "roundtrip_video";
        case StageKind::AnimateStill: return "animate_still";
        case StageKind::Measure: return "measure";
        case StageKind::Compare: return "compare";
        case StageKind::RecompressionCurve: return "recompression_curve";
        case StageKind::CompressionHistory: return "compression_history";
        }
        return "unknown";
    }

    StageKind stage_kind_from_string(const std::string_view name)
    {
        if (name == "decode_image") { return StageKind::DecodeImage; }
        if (name == "decode_video") { return StageKind::DecodeVideo; }
        if (name == "convert") { return StageKind::Convert; }
        if (name == "chroma_roundtrip") { return StageKind::ChromaRoundtrip; }
        if (name == "reinterpret") { return StageKind::Reinterpret; }
        if (name == "crop") { return StageKind::Crop; }
        if (name == "orient") { return StageKind::Orient; }
        if (name == "achromatic") { return StageKind::Achromatic; }
        if (name == "resize") { return StageKind::Resize; }
        if (name == "filter") { return StageKind::Filter; }
        if (name == "encode_image") { return StageKind::EncodeImage; }
        if (name == "encode_video") { return StageKind::EncodeVideo; }
        if (name == "roundtrip_image") { return StageKind::RoundtripImage; }
        if (name == "roundtrip_video") { return StageKind::RoundtripVideo; }
        if (name == "animate_still") { return StageKind::AnimateStill; }
        if (name == "measure") { return StageKind::Measure; }
        if (name == "compare") { return StageKind::Compare; }
        if (name == "recompression_curve") { return StageKind::RecompressionCurve; }
        if (name == "compression_history") { return StageKind::CompressionHistory; }
        throw ConfigError("unknown stage kind '" + std::string(name) + "'");
    }

    std::string to_string(const PictureType type)
    {
        switch (type)
        {
        case PictureType::Unknown: return "unknown";
        case PictureType::I: return "I";
        case PictureType::P: return "P";
        case PictureType::B: return "B";
        }
        return "unknown";
    }

    PictureType picture_type_from_string(const std::string_view name)
    {
        if (name == "I") { return PictureType::I; }
        if (name == "P") { return PictureType::P; }
        if (name == "B") { return PictureType::B; }
        if (name == "unknown") { return PictureType::Unknown; }
        throw ConfigError("unknown picture type '" + std::string(name) + "'");
    }

    // -----------------------------------------------------------------------
    // FormatDescription
    // -----------------------------------------------------------------------

    json::Value FormatDescription::to_json() const
    {
        return json::object({
            {"width", width},
            {"height", height},
            {"pixel_format", pixel_format.to_json()},
            {"color", color.to_json()},
            {"icc_profile", icc_profile},
            {"orientation", json::optional_or_null(orientation)},
            {"sample_aspect_ratio", sample_aspect_ratio.to_json()},
        });
    }

    FormatDescription FormatDescription::from_json(const json::Value& value)
    {
        FormatDescription format;
        format.width = static_cast<int>(value.at("width").get<std::int64_t>());
        format.height = static_cast<int>(value.at("height").get<std::int64_t>());
        format.pixel_format = PixelFormat::from_json(value.at("pixel_format"));
        format.color = ColorSpec::from_json(value.at("color"));
        format.icc_profile = value.at("icc_profile").get<std::string>();
        format.orientation = json::optional_int32(value.at("orientation"));
        format.sample_aspect_ratio = Rational::from_json(value.at("sample_aspect_ratio"));
        return format;
    }

    bool FormatDescription::has_same_samples_as(const FormatDescription& other) const noexcept
    {
        return width == other.width && height == other.height && pixel_format == other.pixel_format &&
               color == other.color;
    }

    bool operator==(const FormatDescription& left, const FormatDescription& right) noexcept
    {
        return left.has_same_samples_as(right) && left.icc_profile == right.icc_profile &&
               left.orientation == right.orientation && left.sample_aspect_ratio == right.sample_aspect_ratio;
    }

    // -----------------------------------------------------------------------
    // FrameStats
    // -----------------------------------------------------------------------

    json::Value FrameStats::to_json() const
    {
        return json::object({
            {"index", index},
            {"pts", pts},
            {"picture_type", to_string(picture_type)},
            {"key_frame", key_frame},
            {"size_bytes", json::optional_or_null(size_bytes)},
            {"qp_min", json::optional_or_null(qp_min)},
            {"qp_max", json::optional_or_null(qp_max)},
            {"qp_mean", json::optional_or_null(qp_mean)},
        });
    }

    FrameStats FrameStats::from_json(const json::Value& value)
    {
        FrameStats stats;
        stats.index = static_cast<int>(value.at("index").get<std::int64_t>());
        stats.pts = value.at("pts").get<std::int64_t>();
        stats.picture_type =
            picture_type_from_string(value.at("picture_type").get<std::string>());
        stats.key_frame = value.at("key_frame").get<bool>();
        stats.size_bytes = json::optional_int(json::member(value, "size_bytes"));
        stats.qp_min = json::optional_double(json::member(value, "qp_min"));
        stats.qp_max = json::optional_double(json::member(value, "qp_max"));
        stats.qp_mean = json::optional_double(json::member(value, "qp_mean"));
        return stats;
    }

    // -----------------------------------------------------------------------
    // StageRecord
    // -----------------------------------------------------------------------

    json::Value StageRecord::to_json() const
    {
        return json::object({
            {"kind", to_string(kind())},
            {"implementation", implementation},
            {"evidence", evidence_to_json(evidence)},
            {"modifies_state", modifies_state},
            {"input", input.to_json()},
            {"output", output.to_json()},
            {"conversions", json::to_array(conversions)},
            {"transform", transform.to_json()},
            {"block_grid", json::optional_or_null(block_grid)},
            {"frames", json::to_array(frames)},
            {"seed", json::optional_or_null(seed)},
            {"reproducible", reproducible},
            {"duration_ms", duration_ms},
            {"ffmpeg_duration_ms", ffmpeg_duration_ms},
        });
    }

    StageRecord StageRecord::from_json(const json::Value& value)
    {
        StageRecord record;
        const StageKind kind = stage_kind_from_string(value.at("kind").get<std::string>());
        record.implementation = json::string_or(value, "implementation", "");
        record.evidence = evidence_from_json(kind, value.at("evidence"));
        record.modifies_state = json::bool_or(value, "modifies_state", true);
        record.input = FormatDescription::from_json(value.at("input"));
        record.output = FormatDescription::from_json(value.at("output"));

        record.conversions = json::from_array<ConversionEvent>(value.at("conversions"));
        record.transform = CoordinateTransform::from_json(value.at("transform"));
        record.block_grid = json::optional_object<BlockGrid>(json::member(value, "block_grid"));
        record.frames = json::from_array<FrameStats>(value.at("frames"));

        const std::optional<std::int64_t> seed = json::optional_int(json::member(value, "seed"));
        if (seed.has_value())
        {
            record.seed = static_cast<std::uint64_t>(*seed);
        }

        record.reproducible = json::bool_or(value, "reproducible", true);
        record.duration_ms = json::double_or(value, "duration_ms", 0.0);
        record.ffmpeg_duration_ms = json::double_or(value, "ffmpeg_duration_ms", 0.0);
        return record;
    }

    // -----------------------------------------------------------------------
    // ProcessingRecord
    // -----------------------------------------------------------------------

    ProcessingRecord ProcessingRecord::for_this_build()
    {
        ProcessingRecord record;
        record.m_build = build_info().to_json();
        record.m_diagnostics = lossylab::diagnostics().to_json();
        return record;
    }

    void ProcessingRecord::append(StageRecord stage, StageConfiguration configuration)
    {
        if (stage_kind(configuration) != stage.kind())
        {
            throw ConfigError("a " + to_string(stage.kind()) + " stage cannot be appended with the configuration "
                              "of a " + to_string(stage_kind(configuration)) + " stage");
        }
        m_stages.push_back(std::move(stage));
        m_configurations.push_back(std::move(configuration));
    }

    void ProcessingRecord::set_origin(std::optional<ProbeResult> origin)
    {
        m_origin = std::move(origin);
    }

    CoordinateTransform ProcessingRecord::end_to_end_transform() const
    {
        CoordinateTransform combined = CoordinateTransform::identity();
        for (const StageRecord& stage : m_stages)
        {
            combined = combined.then(stage.transform);
        }
        return combined;
    }

    std::optional<BlockGrid> ProcessingRecord::effective_block_grid() const
    {
        std::optional<BlockGrid> grid;
        for (const StageRecord& stage : m_stages)
        {
            if (stage.block_grid.has_value())
            {
                // A compression stage imposes a fresh grid, already expressed in
                // its own output coordinates. Any earlier grid is replaced: what
                // survives re-encoding is the newest grid, not the oldest.
                grid = *stage.block_grid;
            }
            else if (grid.has_value())
            {
                grid = grid->apply_transform(stage.transform);
            }
        }
        return grid;
    }

    bool ProcessingRecord::is_reproducible() const noexcept
    {
        for (const StageRecord& stage : m_stages)
        {
            if (!stage.reproducible)
            {
                return false;
            }
        }
        return true;
    }

    int ProcessingRecord::compression_generations() const noexcept
    {
        int generations = 0;
        for (const StageRecord& stage : m_stages)
        {
            switch (stage.kind())
            {
            case StageKind::EncodeImage:
            case StageKind::EncodeVideo:
            case StageKind::RoundtripImage:
            case StageKind::RoundtripVideo:
                ++generations;
                break;
            default:
                break;
            }
        }
        return generations;
    }

    int ProcessingRecord::total_compression_generations() const noexcept
    {
        static constexpr std::array<std::string_view, 11> lossy_codecs = {
            "mjpeg", "h264", "hevc", "vp8", "vp9", "av1", "mpeg1video", "mpeg2video", "mpeg4", "theora", "prores"};

        int generations = compression_generations();
        if (m_origin.has_value())
        {
            if (const StreamInfo* stream = m_origin->primary_video_stream())
            {
                if (std::find(lossy_codecs.begin(), lossy_codecs.end(), stream->codec_name) != lossy_codecs.end())
                {
                    ++generations;
                }
            }
        }
        return generations;
    }

    ConversionList ProcessingRecord::all_conversions() const
    {
        ConversionList all;
        for (const StageRecord& stage : m_stages)
        {
            all.insert(all.end(), stage.conversions.begin(), stage.conversions.end());
        }
        return all;
    }

    void ProcessingRecord::validate_continuity() const
    {
        for (std::size_t i = 1; i < m_stages.size(); ++i)
        {
            const FormatDescription& previous = m_stages[i - 1].output;
            const FormatDescription& current = m_stages[i].input;
            if (previous != current)
            {
                throw ConfigError(
                    "processing record is discontinuous between stage " + std::to_string(i - 1) +
                    " (" + to_string(m_stages[i - 1].kind()) + ") and stage " + std::to_string(i) +
                    " (" + to_string(m_stages[i].kind()) + "): a conversion happened outside the record");
            }
        }
    }

    json::Value ProcessingRecord::to_json() const
    {
        // The origin inside carries no schema version of its own.
        return json::object({
            {"schema_version", schema_version},
            {"build", m_build},
            {"diagnostics", m_diagnostics},
            {"origin", m_origin.has_value() ? reflect::to_json(*m_origin) : json::Value()},
            {"seed", m_seed.has_value() ? json::Value(*m_seed) : json::Value()},
            {"output_sha256", m_output_sha256.has_value() ? json::Value(*m_output_sha256) : json::Value()},
            {"configurations", json::to_array(m_configurations, configuration_to_json)},
            {"stages", json::to_array(m_stages)},
        });
    }

    namespace
    {
        /// The oldest layout from_json() still reads.
        constexpr int oldest_supported_schema_version = 15;

        /// The EncodeImageOptions a stage ran with when it is an image encode or roundtrip, else nullptr.
        const EncodeImageOptions* image_encode_options(const StageRecord& stage,
                                                       const StageConfiguration& configuration) noexcept
        {
            switch (stage.kind())
            {
            case StageKind::EncodeImage: return std::get_if<EncodeImageOptions>(&configuration);
            case StageKind::RoundtripImage:
                if (const auto* roundtrip = std::get_if<RoundtripImageConfiguration>(&configuration))
                {
                    return &roundtrip->encode;
                }
                return nullptr;
            default: return nullptr;
            }
        }

        /// Schema 15 recorded a JPEG or MJPEG encode's grid as the 8x8 transform block, whatever the
        /// subsampling. Schema 16 records the minimum coded unit, as a decode of the same bytes does. The
        /// subsampling is the encoder's input pixel format, which the stage's configuration holds. A crop
        /// that followed such a stage measured itself against the old grid, so its evidence is redone.
        void migrate_schema_15_to_16(std::vector<StageRecord>& stages,
                                     const std::vector<StageConfiguration>& configurations)
        {
            ProcessingRecord earlier;
            bool migrated_grid = false;
            for (std::size_t i = 0; i < stages.size(); ++i)
            {
                StageRecord& stage = stages[i];
                if (const EncodeImageOptions* encode = image_encode_options(stage, configurations[i]);
                    encode != nullptr && stage.block_grid.has_value() && stage.block_grid->kind == BlockGridKind::Dct8 &&
                    (encode->codec == ImageCodec::Jpeg || encode->codec == ImageCodec::Mjpeg))
                {
                    stage.block_grid = BlockGrid::jpeg_mcu(encode->pixel_format.log2_chroma_width(),
                                                           encode->pixel_format.log2_chroma_height());
                    migrated_grid = true;
                }
                else if (const auto* crop = std::get_if<CropOptions>(&configurations[i]);
                         migrated_grid && crop != nullptr && std::holds_alternative<CropEvidence>(stage.evidence))
                {
                    if (const std::optional<BlockGrid> grid = earlier.effective_block_grid(); grid.has_value())
                    {
                        CropEvidence evidence;
                        evidence.block_grid = grid->apply_transform(stage.transform);
                        evidence.whole_blocks =
                            crop->width % grid->block_width == 0 && crop->height % grid->block_height == 0;
                        stage.evidence = evidence;
                    }
                }
                earlier.append(stage, configurations[i]);
            }
        }
    }

    ProcessingRecord ProcessingRecord::from_json(const json::Value& value)
    {
        const int version = static_cast<int>(value.at("schema_version").get<std::int64_t>());
        if (version < oldest_supported_schema_version || version > schema_version)
        {
            throw ConfigError("processing record has schema_version " + std::to_string(version) +
                              "; this build reads " + std::to_string(oldest_supported_schema_version) + " to " +
                              std::to_string(schema_version));
        }

        ProcessingRecord record;
        record.m_build = json::member(value, "build");
        record.m_diagnostics = json::member(value, "diagnostics");
        const json::Value& origin = value.at("origin");
        if (!origin.is_null())
        {
            record.set_origin(ProbeResult::from_json(origin));
        }
        if (const json::Value& seed = value.at("seed"); !seed.is_null())
        {
            record.m_seed = seed.get<std::uint64_t>();
        }
        if (const json::Value& output = value.at("output_sha256"); !output.is_null())
        {
            record.m_output_sha256 = output.get<std::string>();
        }
        std::vector<StageRecord> stages = json::from_array<StageRecord>(value.at("stages"));
        const json::Value& configurations = value.at("configurations");
        if (configurations.size() != stages.size())
        {
            throw ConfigError("processing record has " + std::to_string(stages.size()) + " stages but " +
                              std::to_string(configurations.size()) + " configurations");
        }
        std::vector<StageConfiguration> parsed_configurations;
        for (std::size_t i = 0; i < stages.size(); ++i)
        {
            parsed_configurations.push_back(configuration_from_json(stages[i].kind(), configurations[i]));
        }
        if (version == 15)
        {
            migrate_schema_15_to_16(stages, parsed_configurations);
        }
        for (std::size_t i = 0; i < stages.size(); ++i)
        {
            record.append(std::move(stages[i]), std::move(parsed_configurations[i]));
        }
        return record;
    }
}
