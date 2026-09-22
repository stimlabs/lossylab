#include "lossylab/core/record.hpp"

#include "lossylab/core/error.hpp"
#include "lossylab/core/json_io.hpp"

#include <utility>

namespace lossylab
{
    std::string to_string(const StageKind kind)
    {
        switch (kind)
        {
        case StageKind::Decode: return "decode";
        case StageKind::Probe: return "probe";
        case StageKind::Convert: return "convert";
        case StageKind::ChromaRoundtrip: return "chroma_roundtrip";
        case StageKind::Reinterpret: return "reinterpret";
        case StageKind::Resize: return "resize";
        case StageKind::Filter: return "filter";
        case StageKind::EncodeImage: return "encode_image";
        case StageKind::EncodeVideo: return "encode_video";
        case StageKind::Roundtrip: return "roundtrip";
        case StageKind::AnimateStill: return "animate_still";
        case StageKind::Measure: return "measure";
        case StageKind::Compare: return "compare";
        }
        return "unknown";
    }

    StageKind stage_kind_from_string(const std::string_view name)
    {
        if (name == "decode") { return StageKind::Decode; }
        if (name == "probe") { return StageKind::Probe; }
        if (name == "convert") { return StageKind::Convert; }
        if (name == "chroma_roundtrip") { return StageKind::ChromaRoundtrip; }
        if (name == "reinterpret") { return StageKind::Reinterpret; }
        if (name == "resize") { return StageKind::Resize; }
        if (name == "filter") { return StageKind::Filter; }
        if (name == "encode_image") { return StageKind::EncodeImage; }
        if (name == "encode_video") { return StageKind::EncodeVideo; }
        if (name == "roundtrip") { return StageKind::Roundtrip; }
        if (name == "animate_still") { return StageKind::AnimateStill; }
        if (name == "measure") { return StageKind::Measure; }
        if (name == "compare") { return StageKind::Compare; }
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
            {"pix_fmt", pixel_format.to_json()},
            {"color", color.to_json()},
        });
    }

    FormatDescription FormatDescription::from_json(const json::Value& value)
    {
        FormatDescription format;
        format.width = static_cast<int>(value.at("width").get<std::int64_t>());
        format.height = static_cast<int>(value.at("height").get<std::int64_t>());
        format.pixel_format = PixelFormat::from_json(value.at("pix_fmt"));
        format.color = ColorSpec::from_json(value.at("color"));
        return format;
    }

    bool operator==(const FormatDescription& left, const FormatDescription& right) noexcept
    {
        return left.width == right.width && left.height == right.height &&
               left.pixel_format == right.pixel_format && left.color == right.color;
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
            {"kind", to_string(kind)},
            {"implementation", implementation},
            {"params", params},
            {"input", input.to_json()},
            {"output", output.to_json()},
            {"conversions", json::to_array(conversions)},
            {"transform", transform.to_json()},
            {"block_grid", json::optional_or_null(block_grid)},
            {"frames", json::to_array(frames)},
            {"encoder_settings", encoder_settings},
            {"achieved_bpp", json::optional_or_null(achieved_bpp)},
            {"seed", json::optional_or_null(seed)},
            {"reproducible", reproducible},
            {"duration_ms", duration_ms},
        });
    }

    StageRecord StageRecord::from_json(const json::Value& value)
    {
        StageRecord record;
        record.kind = stage_kind_from_string(value.at("kind").get<std::string>());
        record.implementation = json::string_or(value, "implementation", "");
        record.params = json::member(value, "params");
        record.input = FormatDescription::from_json(value.at("input"));
        record.output = FormatDescription::from_json(value.at("output"));

        record.conversions = json::from_array<ConversionEvent>(value.at("conversions"));
        record.transform = CoordinateTransform::from_json(value.at("transform"));
        record.block_grid = json::optional_object<BlockGrid>(json::member(value, "block_grid"));
        record.frames = json::from_array<FrameStats>(value.at("frames"));

        record.encoder_settings = json::member(value, "encoder_settings");
        record.achieved_bpp = json::optional_double(json::member(value, "achieved_bpp"));

        const std::optional<std::int64_t> seed = json::optional_int(json::member(value, "seed"));
        if (seed.has_value())
        {
            record.seed = static_cast<std::uint64_t>(*seed);
        }

        record.reproducible = json::bool_or(value, "reproducible", true);
        record.duration_ms = json::double_or(value, "duration_ms", 0.0);
        return record;
    }

    // -----------------------------------------------------------------------
    // ProcessingRecord
    // -----------------------------------------------------------------------

    ProcessingRecord::ProcessingRecord(std::string build_id) : m_build_id(std::move(build_id)) {}

    void ProcessingRecord::append(StageRecord stage)
    {
        m_stages.push_back(std::move(stage));
    }

    void ProcessingRecord::set_build_id(std::string build_id)
    {
        m_build_id = std::move(build_id);
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
            switch (stage.kind)
            {
            case StageKind::EncodeImage:
            case StageKind::EncodeVideo:
            case StageKind::Roundtrip:
                ++generations;
                break;
            default:
                break;
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
                    " (" + to_string(m_stages[i - 1].kind) + ") and stage " + std::to_string(i) +
                    " (" + to_string(m_stages[i].kind) + "): a conversion happened outside the record");
            }
        }
    }

    json::Value ProcessingRecord::to_json() const
    {
        return json::object({
            {"build_id", m_build_id},
            {"stages", json::to_array(m_stages)},
        });
    }

    ProcessingRecord ProcessingRecord::from_json(const json::Value& value)
    {
        ProcessingRecord record(json::string_or(value, "build_id", ""));
        for (StageRecord& stage : json::from_array<StageRecord>(value.at("stages")))
        {
            record.append(std::move(stage));
        }
        return record;
    }
}
