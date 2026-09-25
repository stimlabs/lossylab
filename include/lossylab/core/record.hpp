#pragma once

#include "lossylab/core/color_spec.hpp"
#include "lossylab/core/geometry.hpp"
#include "lossylab/core/json.hpp"
#include "lossylab/core/pixel_format.hpp"
#include "lossylab/core/rational.hpp"
#include "lossylab/core/reflect.hpp"
#include "lossylab/core/strict.hpp"
#include "lossylab/io/probe.hpp"

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

        /// The name of the ICC profile the frame carries (see
        /// `IccProfileInfo::name()`); empty when it carries none.
        std::string icc_profile;

        /// The EXIF orientation the frame carries but has not applied.
        std::optional<int> orientation;

        /// The shape of one pixel, width to height.
        Rational sample_aspect_ratio{1, 1};

        /// True when both describe samples of the same size, pixel format and
        /// color, whatever ICC profile, orientation or pixel shape they carry.
        [[nodiscard]] bool has_same_samples_as(const FormatDescription& other) const noexcept;

        [[nodiscard]] json::Value to_json() const;
        static FormatDescription from_json(const json::Value& value);
    };

    bool operator==(const FormatDescription& left, const FormatDescription& right) noexcept;
    inline bool operator!=(const FormatDescription& left, const FormatDescription& right) noexcept
    {
        return !(left == right);
    }

    LOSSYLAB_REFLECT(FormatDescription, width, height, pixel_format, color, icc_profile, orientation,
                      sample_aspect_ratio);

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
        // TODO: qp_std

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

        /// Per-file evidence: values this stage resolved from the file it
        /// processed (what was measured as, what was detected, which stream was
        /// read). The options the stage ran with are not here; they are the
        /// same for every file and live in `ProcessingRecord::configurations()`.
        json::Value params;

        /// True when this stage's output is the input of the next stage, so
        /// replaying the chain has to run it. False for a stage that only
        /// analyzes: any conversion in its `conversions` was applied to a copy
        /// the stage discarded.
        bool modifies_state = true;

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

        /// The part of `duration_ms` spent inside FFmpeg calls (opening,
        /// reading, decoding, encoding, muxing, scaling, filtering);
        /// `duration_ms - ffmpeg_duration_ms` is this library's own overhead.
        double ffmpeg_duration_ms = 0.0;

        [[nodiscard]] json::Value to_json() const;
        static StageRecord from_json(const json::Value& value);
    };

    LOSSYLAB_REFLECT(StageRecord, kind, implementation, params, modifies_state, input, output, conversions, transform,
                      block_grid, frames, encoder_settings, achieved_bpp, seed, reproducible, duration_ms,
                      ffmpeg_duration_ms);

    /// The processing history of a frame or clip: every stage, in order.
    ///
    /// JSON-serializable, so it can be stored beside a sample, compared across
    /// data sources, and used to stratify or filter a dataset after the fact.
    class ProcessingRecord
    {
    public:
        ProcessingRecord() = default;

        /// A record for processing done by this build: it carries this build's
        /// identity (see `build_info()`) and the machine (see `diagnostics()`).
        [[nodiscard]] static ProcessingRecord for_this_build();

        /// Adds a stage together with the options it ran with (`Strict` mode,
        /// kernels, thresholds, encoder options, after defaults and
        /// randomization were resolved). The options are static for the whole
        /// record: a run with other options or another build is a new record.
        void append(StageRecord stage, json::Value configuration = json::Value::object());

        [[nodiscard]] const std::vector<StageRecord>& stages() const noexcept { return m_stages; }

        /// The options of each stage, at the same position as in `stages()`.
        /// Replaying stage `i` is applying `configurations()[i]` to the output
        /// of every earlier stage that modifies state.
        [[nodiscard]] const std::vector<json::Value>& configurations() const noexcept { return m_configurations; }
        [[nodiscard]] bool empty() const noexcept { return m_stages.empty(); }
        [[nodiscard]] std::size_t size() const noexcept { return m_stages.size(); }

        /// The identity of the build that produced the record (`BuildInfo::to_json()`),
        /// or null for a record that was not made by a build. Two records of the
        /// same input that disagree can be traced with `build_diff()` of the two.
        [[nodiscard]] const json::Value& build() const noexcept { return m_build; }

        /// The machine it was produced on (`Diagnostics::to_json()`), or null.
        [[nodiscard]] const json::Value& diagnostics() const noexcept { return m_diagnostics; }

        /// What probe() reports for the file the first stage decoded, when the
        /// history starts from one (see `DecodedImage::processing_record()`):
        /// the facts about the file that no stage changes, such as its
        /// container, encoder and JPEG tables.
        [[nodiscard]] const std::optional<ProbeResult>& origin() const noexcept { return m_origin; }
        void set_origin(std::optional<ProbeResult> origin);

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
        json::Value m_build;
        json::Value m_diagnostics;
        std::optional<ProbeResult> m_origin;
        std::vector<StageRecord> m_stages;
        std::vector<json::Value> m_configurations;
    };
}
