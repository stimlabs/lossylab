#pragma once

#include "lossylab/core/color_spec.hpp"
#include "lossylab/core/geometry.hpp"
#include "lossylab/core/json.hpp"
#include "lossylab/core/pixel_format.hpp"
#include "lossylab/core/reflect.hpp"
#include "lossylab/core/strict.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lossylab
{
    /// What kind of operation a stage performed. One member per operation the
    /// design specifies, so a record can be grouped and compared by stage type
    /// across a whole dataset.
    enum class StageKind
    {
        Decode,
        Probe,
        Convert,
        ChromaRoundtrip,
        Reinterpret,
        Resize,
        Filter,
        EncodeImage,
        EncodeVideo,
        Roundtrip,
        AnimateStill,
        Measure,
        Compare,
        RecompressionCurve,
        CompressionHistory
    };

    std::string to_string(StageKind kind);
    StageKind stage_kind_from_string(std::string_view name);

    /// Coded picture type. Frame type drives compression severity, so training
    /// samples are stratified on it and audits report its distribution.
    enum class PictureType
    {
        Unknown,
        I,
        P,
        B
    };

    std::string to_string(PictureType type);
    PictureType picture_type_from_string(std::string_view name);

    /// The complete format of a frame at a stage boundary. Every stage states
    /// both its input and its output, so a chain can be checked for a silent
    /// mismatch after the fact, not only while it runs.
    struct FormatDescription
    {
        int width = 0;
        int height = 0;
        PixelFormat pixel_format;
        ColorSpec color;

        [[nodiscard]] json::Value to_json() const;
        static FormatDescription from_json(const json::Value& value);
    };

    bool operator==(const FormatDescription& left, const FormatDescription& right) noexcept;
    inline bool operator!=(const FormatDescription& left, const FormatDescription& right) noexcept
    {
        return !(left == right);
    }

    LOSSYLAB_REFLECT(FormatDescription, width, height, pixel_format, color);

    /// Per-frame statistics from an encode or a decode.
    struct FrameStats
    {
        int index = 0;
        std::int64_t pts = 0;
        PictureType picture_type = PictureType::Unknown;
        bool key_frame = false;

        /// Coded size in bytes, when the stage encoded this frame.
        std::optional<std::int64_t> size_bytes;

        /// Quantizer statistics, where the codec exposes them. These are the
        /// per-frame compression strength an audit reports and a crop-level
        /// severity estimate is built from.
        std::optional<double> qp_min;
        std::optional<double> qp_max;
        std::optional<double> qp_mean;

        [[nodiscard]] json::Value to_json() const;
        static FrameStats from_json(const json::Value& value);
    };

    LOSSYLAB_REFLECT(FrameStats, index, pts, picture_type, key_frame, size_bytes, qp_min, qp_max, qp_mean);

    /// What one operation did.
    ///
    /// This is the unit that makes two classes of data comparable: if both
    /// received the same distribution of stage records, they received the same
    /// processing, whatever the pipeline looked like.
    struct StageRecord
    {
        StageKind kind = StageKind::Convert;

        /// The concrete implementation, e.g. "swscale", "zscale", "libx264".
        std::string implementation;

        /// Parameters after defaults and randomization were resolved, never as
        /// the caller passed them. Replaying these reproduces the stage.
        json::Value params;

        FormatDescription input;
        FormatDescription output;

        /// Conversions that actually took place. Empty under Strict::Refuse,
        /// because anything implicit would have thrown instead.
        ConversionList conversions;

        /// Maps input coordinates to output coordinates for this stage alone.
        CoordinateTransform transform;

        /// The coding block grid this stage imposed, in its own output
        /// coordinates. Set by compression stages; absent for everything else.
        std::optional<BlockGrid> block_grid;

        std::vector<FrameStats> frames;

        /// Encoder configuration as resolved, including rate control and GOP
        /// structure. Separate from `params` because it is the part that has to
        /// match across classes for an equalization to be honest.
        json::Value encoder_settings;

        /// Achieved bits per pixel, for compression stages. The unit that makes
        /// severity comparable between codecs.
        std::optional<double> achieved_bpp;

        /// The seed this stage drew from, when it was stochastic.
        std::optional<std::uint64_t> seed;

        /// False when the stage cannot be reproduced bit-exactly: hardware
        /// encoders, or any path whose output depends on thread scheduling.
        bool reproducible = true;

        /// Wall-clock cost, for profiling a pipeline.
        double duration_ms = 0.0;

        [[nodiscard]] json::Value to_json() const;
        static StageRecord from_json(const json::Value& value);
    };

    LOSSYLAB_REFLECT(StageRecord, kind, implementation, params, input, output, conversions, transform, block_grid,
                      frames, encoder_settings, achieved_bpp, seed, reproducible, duration_ms);

    /// The processing history of a frame or clip: every stage, in order.
    ///
    /// JSON-serializable, so it can be stored beside a sample, compared across
    /// data sources, and used to stratify or filter a dataset after the fact.
    class ProcessingRecord
    {
    public:
        ProcessingRecord() = default;
        explicit ProcessingRecord(std::string build_id);

        void append(StageRecord stage);

        [[nodiscard]] const std::vector<StageRecord>& stages() const noexcept { return m_stages; }
        [[nodiscard]] bool empty() const noexcept { return m_stages.empty(); }
        [[nodiscard]] std::size_t size() const noexcept { return m_stages.size(); }

        [[nodiscard]] const std::string& build_id() const noexcept { return m_build_id; }
        void set_build_id(std::string build_id);

        /// The composition of every stage transform: maps a coordinate in the
        /// original input to the corresponding coordinate in the final output.
        /// Inverting it is how an output crop or an inpainting mask is traced
        /// back to the source.
        [[nodiscard]] CoordinateTransform end_to_end_transform() const;

        /// The block grid still discernible in the final output, if any.
        ///
        /// Walks the stages in order: a compression stage establishes a grid,
        /// and every later stage's transform is applied to it, which is what
        /// marks a grid destroyed once something resamples.
        [[nodiscard]] std::optional<BlockGrid> effective_block_grid() const;

        /// True when every stage is reproducible, i.e. replaying the record
        /// with the same seeds yields identical output.
        [[nodiscard]] bool is_reproducible() const noexcept;

        /// Total bits per pixel across compression stages, and the number of
        /// compression generations the data has been through.
        [[nodiscard]] int compression_generations() const noexcept;

        /// Every conversion across every stage, flattened, in order.
        [[nodiscard]] ConversionList all_conversions() const;

        /// Checks that each stage's input format equals the previous stage's
        /// output format, throwing ConfigError on the first mismatch. A chain
        /// that fails this had a conversion happen outside the record.
        void validate_continuity() const;

        [[nodiscard]] json::Value to_json() const;
        static ProcessingRecord from_json(const json::Value& value);

    private:
        std::string m_build_id;
        std::vector<StageRecord> m_stages;
    };
}
