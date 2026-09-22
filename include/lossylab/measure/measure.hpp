#pragma once

#include "lossylab/core/codec_id.hpp"
#include "lossylab/core/frame.hpp"
#include "lossylab/core/json.hpp"
#include "lossylab/core/record.hpp"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace lossylab
{
    /// A no-reference statistic computable from a frame alone.
    ///
    /// These are what an audit runs over a dataset it did not create, where
    /// there is no pristine original to compare against.
    enum class Analyzer
    {
        /// Minimum, maximum and histogram of luma and chroma. Reveals limited
        /// versus full range directly, and catches the levels mismatch that a
        /// mislabeled range leaves behind.
        SignalLevels,

        /// Blockiness: energy concentrated on a regular grid. A direct measure
        /// of block-transform compression strength.
        Blockiness,

        /// Blurriness, from high-frequency energy.
        Blurriness,

        /// Black bars around the image, with the detected content rectangle.
        /// Tells crop sampling where not to crop, which otherwise quietly
        /// produces training crops of pure black.
        Letterbox,

        /// Interlacing and telecine patterns.
        Interlacing,

        /// Spatial and temporal information, the ITU-T P.910 complexity
        /// measures. Used to stratify so that two classes are compared at
        /// similar content complexity.
        SpatialTemporalInfo,

        /// Scene change score per frame.
        SceneChange,

        /// Frames identical to their predecessor, which indicate frame rate
        /// conversion or a still padded out to a clip.
        DuplicateFrames
    };

    std::string to_string(Analyzer analyzer);
    Analyzer analyzer_from_string(std::string_view name);

    /// One frame's measurements.
    struct FrameMeasurement
    {
        int index = 0;

        /// Keyed by a name specific to the analyzer, e.g. "luma_min",
        /// "blockiness", "si", "ti", "scene_score".
        std::map<std::string, double> values;

        /// The content rectangle from letterbox detection.
        std::optional<Rect> content_rect;

        [[nodiscard]] std::optional<double> value(std::string_view name) const;

        [[nodiscard]] json::Value to_json() const;
    };

    struct MeasureResult
    {
        std::vector<FrameMeasurement> frames;

        /// Values pooled across frames: mean, min and max of each measurement.
        std::map<std::string, double> pooled;

        StageRecord record;

        [[nodiscard]] json::Value to_json() const;
    };

    /// Runs no-reference analyzers over frames.
    [[nodiscard]] MeasureResult measure(const std::vector<Frame>& frames,
                                        const std::vector<Analyzer>& analyzers);

    /// One frame, for convenience.
    [[nodiscard]] MeasureResult measure(const Frame& frame,
                                        const std::vector<Analyzer>& analyzers);

    /// Full-reference comparison between a reference and a distorted version.
    struct CompareResult
    {
        /// Per frame, keyed by metric name.
        std::vector<std::map<std::string, double>> frames;

        /// Pooled across frames. PSNR pools as a mean of per-frame values,
        /// which is the convention; note that this is not the same as PSNR of
        /// the pooled MSE, and the two disagree on clips with varying quality.
        std::map<std::string, double> pooled;

        StageRecord record;

        [[nodiscard]] json::Value to_json() const;
    };

    /// Compares distorted frames against a reference.
    ///
    /// Used to calibrate severity for `encode_to_target` and to record how
    /// strong a degradation actually was, rather than how strong its parameters
    /// suggested it would be.
    [[nodiscard]] CompareResult compare(const std::vector<Frame>& reference,
                                        const std::vector<Frame>& distorted,
                                        const std::vector<Metric>& metrics);

    [[nodiscard]] CompareResult compare(const Frame& reference, const Frame& distorted,
                                        const std::vector<Metric>& metrics);

    /// One point on a recompression sweep.
    struct RecompressionPoint
    {
        double quality_parameter = 0.0;

        /// Error between the input and its re-encoding at this parameter.
        double error = 0.0;

        /// Bits per pixel the re-encode achieved.
        double bits_per_pixel = 0.0;

        [[nodiscard]] json::Value to_json() const;
    };

    struct RecompressionCurve
    {
        std::vector<RecompressionPoint> points;

        /// The parameter at the curve's minimum or knee, when one is clear.
        ///
        /// A prior compression with the same codec at similar settings shows up
        /// here: re-encoding at the original quality changes the image least,
        /// because it is already sitting on that codec's quantization lattice.
        /// This is the JPEG "ghost" principle, extended to WebP, AVIF and
        /// intra-coded video.
        std::optional<double> estimated_prior_parameter;

        /// How pronounced the minimum is. A flat curve means no prior
        /// compression was detected, not that there was none.
        double confidence = 0.0;

        StageRecord record;

        [[nodiscard]] json::Value to_json() const;
    };

    struct RecompressionOptions
    {
        ImageCodec codec = ImageCodec::Mjpeg;

        /// Quality parameters to sweep, in the codec's own units.
        std::vector<double> parameter_range;

        /// Metric used to measure the difference at each point.
        Metric metric = Metric::Psnr;

        PixelFormat pixel_format;
        std::optional<ColorSpec> color;
    };

    /// Sweeps a codec's quality parameter, measuring how much re-encoding
    /// changes the input at each setting.
    ///
    /// Experimental. The signal is strong for single-generation JPEG and WebP
    /// and weakens quickly with further processing; a resize after the original
    /// compression usually destroys it entirely.
    [[nodiscard]] RecompressionCurve recompression_curve(const Frame& frame,
                                                         const RecompressionOptions& options);
}
