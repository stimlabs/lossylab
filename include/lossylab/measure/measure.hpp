#pragma once

#include "lossylab/core/codec_id.hpp"
#include "lossylab/core/frame.hpp"
#include "lossylab/core/json.hpp"
#include "lossylab/core/record.hpp"
#include "lossylab/core/reflect.hpp"
#include "lossylab/core/statistics.hpp"
#include "lossylab/core/strict.hpp"
#include "lossylab/measure/measure_types.hpp"

#include <variant>
#include <vector>

namespace lossylab
{
    struct MeasureResult
    {
        /// Its evidence holds the measurements (see `evidence()`).
        StageRecord record;

        /// What the measurement was told to do, resolved (see `FrameResult::configuration`).
        MeasureOptions configuration;

        /// The measurements: per frame, pooled, and the format each analyzer
        /// measured in.
        [[nodiscard]] const MeasureEvidence& evidence() const { return std::get<MeasureEvidence>(record.evidence); }

        [[nodiscard]] json::Value to_json() const;
    };

    /// Runs no-reference analyzers over frames.
    ///
    /// The frames must share one size, pixel format and color, and the
    /// options must name at least one analyzer. Interlacing,
    /// SpatialTemporalInfo, SceneChange and DuplicateFrames are not implemented
    /// yet and throw NotImplemented.
    [[nodiscard]] MeasureResult measure(const std::vector<Frame>& frames, const MeasureOptions& options);

    /// One frame, for convenience.
    [[nodiscard]] MeasureResult measure(const Frame& frame, const MeasureOptions& options);

    /// With the default options otherwise.
    [[nodiscard]] MeasureResult measure(const std::vector<Frame>& frames, const std::vector<Analyzer>& analyzers);
    [[nodiscard]] MeasureResult measure(const Frame& frame, const std::vector<Analyzer>& analyzers);

    /// Full-reference comparison between a reference and a distorted version.
    struct CompareResult
    {
        /// Its evidence holds the metric values (see `evidence()`).
        StageRecord record;

        /// What the comparison was told to do, resolved (see `FrameResult::configuration`).
        CompareOptions configuration;

        /// The metric values: per frame, pooled, and the format each metric
        /// measured in.
        [[nodiscard]] const CompareEvidence& evidence() const { return std::get<CompareEvidence>(record.evidence); }

        [[nodiscard]] json::Value to_json() const;
    };

    /// Compares distorted frames against a reference.
    ///
    /// Used to calibrate severity for `encode_to_target` and to record how
    /// strong a degradation actually was, rather than how strong its parameters
    /// suggested it would be. Reference and distorted frames must share one
    /// size, pixel format and color, since a difference in any of them would
    /// be measured as distortion; SSIM needs frames of at least 8x8. The
    /// options must name at least one metric. The evidence lists what each
    /// metric measured under `measured_as`.
    [[nodiscard]] CompareResult compare(const std::vector<Frame>& reference, const std::vector<Frame>& distorted,
                                        const CompareOptions& options);

    [[nodiscard]] CompareResult compare(const Frame& reference, const Frame& distorted,
                                        const CompareOptions& options);

    /// With the default options otherwise.
    [[nodiscard]] CompareResult compare(const std::vector<Frame>& reference, const std::vector<Frame>& distorted,
                                        const std::vector<Metric>& metrics);
    [[nodiscard]] CompareResult compare(const Frame& reference, const Frame& distorted,
                                        const std::vector<Metric>& metrics);

    struct RecompressionCurve
    {
        /// Its evidence holds the curve (see `evidence()`).
        StageRecord record;

        /// What the sweep was told to do, resolved: the format and color
        /// filled in, and the parameters sorted without repeats (see
        /// `FrameResult::configuration`).
        RecompressionOptions configuration;

        /// The curve: its points, its notch and how confident it is.
        [[nodiscard]] const RecompressionCurveEvidence& evidence() const
        {
            return std::get<RecompressionCurveEvidence>(record.evidence);
        }

        [[nodiscard]] json::Value to_json() const;
    };

    /// Sweeps a codec's quality parameter, measuring how much re-encoding
    /// changes the input at each setting.
    ///
    /// Takes a frame in any pixel format; see RecompressionOptions for what
    /// it is re-encoded in. Checked on photos: a WebP ghost shows up at the
    /// original quality with confidence of about 0.5 to 0.9 under Luma
    /// planes, and less for flat, low-detail content. An MJPEG sweep finds
    /// FFmpeg's own MJPEG output exactly, but responds only weakly to a
    /// libjpeg-written JPEG, whose tables no qscale reproduces: on photos
    /// saved at libjpeg quality 50 or 75, qscale 2 to 14 gave notches of
    /// confidence 0.4 to 0.75 at a qscale unrelated to the quality, and at
    /// quality 90 none. compression_history() reads a JPEG's tables
    /// directly instead. AVIF and JPEG XL leave a notch at the original
    /// setting when re-encoded with the same encoder and settings (libaom crf
    /// 11 to 43 and libjxl distance 1 to 4.5 on the test pattern), but for
    /// AVIF a never-compressed frame can show one of similar confidence too,
    /// and neither has been checked against other encoders. JPEG 2000 leaves
    /// a strong notch (confidence 0.6 to 0.97): FFmpeg's own output at
    /// nominal ratio 8 to 60 is found mostly exactly and at worst 25% off,
    /// since several ratios re-encode it unchanged, and OpenJPEG's (through
    /// Pillow) is found too, but its true ratio reads about 2.5 times higher
    /// in FFmpeg's nominal units for RGB, and a little lower for gray. A
    /// resize after the original compression usually destroys the signal
    /// entirely. The evidence holds each point's notch depth.
    [[nodiscard]] RecompressionCurve recompression_curve(const Frame& frame,
                                                         const RecompressionOptions& options);
}
