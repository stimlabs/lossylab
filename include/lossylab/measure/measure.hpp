#pragma once

#include "lossylab/core/codec_id.hpp"
#include "lossylab/core/frame.hpp"
#include "lossylab/core/json.hpp"
#include "lossylab/core/record.hpp"
#include "lossylab/core/reflect.hpp"
#include "lossylab/core/strict.hpp"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace lossylab
{
    /// A no-reference statistic computable from a frame alone.
    ///
    /// These are what an audit runs over a dataset it did not create, where
    /// there is no pristine original to compare against. Values are in the code
    /// values of the format the analyzer measured, which the record lists under
    /// `params["measured_as"]`.
    enum class Analyzer
    {
        /// FFmpeg's signalstats. For luma ("luma_"), the two chroma planes
        /// ("u_", "v_") and saturation ("saturation_"): the minimum, 10th
        /// percentile ("low"), mean, 90th percentile ("high") and maximum.
        /// Also "hue_median", "hue_mean", the bit depth each plane actually
        /// uses ("luma_bit_depth", "u_bit_depth", "v_bit_depth"), and
        /// "outside_limited_range", the fraction of pixels with any component
        /// outside 16-235 luma or 16-240 chroma (scaled to the bit depth).
        /// Reveals limited versus full range directly, and catches the levels
        /// mismatch that a mislabeled range leaves behind. Planar YUV only.
        SignalLevels,

        /// FFmpeg's blockdetect on the first plane: "blockiness", the gradient
        /// energy on the strongest regular grid between 3 and 24 pixels,
        /// relative to the energy off it. About 1 for an image without block
        /// artifacts, rising with block-transform compression strength.
        /// 8-bit formats only.
        Blockiness,

        /// FFmpeg's blurdetect on the first plane: "blurriness", the mean width
        /// in pixels of the edges found by a Canny detector. Grows with blur,
        /// and with resolution for the same content. Absent for a frame with no
        /// edges. 8-bit formats only.
        Blurriness,

        /// Noise level: "noise_sigma", the standard deviation of the noise on
        /// the luma plane in 8-bit code values, whatever the bit depth. Tai and
        /// Yang's variant of Immerkaer's estimator: the response of a
        /// Laplacian-difference kernel, averaged over all pixels except the 10%
        /// with the strongest Sobel gradient, so that edges and texture inflate
        /// it less. Absent for a frame smaller than 3x3.
        Noise,

        /// FFmpeg's cropdetect: the content rectangle inside black bars, found
        /// from rows and columns whose mean luma is at most 24/255 of full
        /// scale. Sets `content_rect`, "letterbox_top", "letterbox_bottom",
        /// "letterbox_left", "letterbox_right" (bar sizes in pixels) and
        /// "content_fraction". A frame that is black throughout has an empty
        /// `content_rect` and a "content_fraction" of 0. Tells crop sampling
        /// where not to crop, which otherwise quietly produces training crops
        /// of pure black.
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

    struct MeasureOptions
    {
        /// What happens when an analyzer does not accept the frames' pixel
        /// format. Under Refuse, measure() throws ConversionRefused naming the
        /// analyzer. Under AllowRecorded, the frames are converted to the
        /// nearest format the analyzer accepts, and the conversion is recorded.
        Strict strict = Strict::Refuse;
    };

    LOSSYLAB_REFLECT(MeasureOptions, strict);

    /// Runs no-reference analyzers over frames.
    ///
    /// The frames must share one size, pixel format and color. Interlacing,
    /// SpatialTemporalInfo, SceneChange and DuplicateFrames are not implemented
    /// yet and throw NotImplemented.
    [[nodiscard]] MeasureResult measure(const std::vector<Frame>& frames,
                                        const std::vector<Analyzer>& analyzers,
                                        const MeasureOptions& options = {});

    /// One frame, for convenience.
    [[nodiscard]] MeasureResult measure(const Frame& frame,
                                        const std::vector<Analyzer>& analyzers,
                                        const MeasureOptions& options = {});

    /// Full-reference comparison between a reference and a distorted version.
    struct CompareResult
    {
        /// Per frame, keyed by metric name. PSNR sets "psnr" (over all planes,
        /// weighted by their sample counts), "mse" (in the code values of the
        /// format it measured), and "psnr_<c>" and "mse_<c>" for each component
        /// c of y, u, v or r, g, b, plus a for alpha. PSNR is infinite for
        /// identical frames, and serializes as null. SSIM sets "ssim" (the
        /// planes weighted as FFmpeg's ssim filter weights them), "ssim_db",
        /// and "ssim_<c>" for each component.
        std::vector<std::map<std::string, double>> frames;

        /// Pooled across frames: the mean, min and max of each value, as
        /// "<name>_mean", "<name>_min" and "<name>_max". "psnr_mean" is the
        /// mean of per-frame PSNR, which is the convention; it is not the PSNR
        /// of the mean MSE, and the two disagree on clips with varying quality.
        std::map<std::string, double> pooled;

        StageRecord record;

        [[nodiscard]] json::Value to_json() const;
    };

    struct CompareOptions
    {
        /// What happens when a metric's filter does not accept the frames'
        /// pixel format (neither takes packed RGB). Under Refuse, compare()
        /// throws ConversionRefused naming the filter. Under AllowRecorded,
        /// both sides are converted to the nearest format it accepts, and the
        /// conversion is recorded.
        Strict strict = Strict::Refuse;
    };

    LOSSYLAB_REFLECT(CompareOptions, strict);

    /// Compares distorted frames against a reference.
    ///
    /// Used to calibrate severity for `encode_to_target` and to record how
    /// strong a degradation actually was, rather than how strong its parameters
    /// suggested it would be. Reference and distorted frames must share one
    /// size, pixel format and color, since a difference in any of them would
    /// be measured as distortion; SSIM needs frames of at least 8x8. The
    /// record's params list what each metric measured under `measured_as`.
    [[nodiscard]] CompareResult compare(const std::vector<Frame>& reference,
                                        const std::vector<Frame>& distorted,
                                        const std::vector<Metric>& metrics,
                                        const CompareOptions& options = {});

    [[nodiscard]] CompareResult compare(const Frame& reference, const Frame& distorted,
                                        const std::vector<Metric>& metrics,
                                        const CompareOptions& options = {});

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
        /// One per swept parameter, in ascending parameter order.
        std::vector<RecompressionPoint> points;

        /// The parameter at the curve's notch, when one is clear.
        ///
        /// A prior compression with the same codec at similar settings shows up
        /// here: re-encoding at the original quality changes the image least,
        /// because it is already sitting on that codec's quantization lattice.
        /// This is the JPEG "ghost" principle, extended to WebP, AVIF and
        /// JPEG XL. Set when `confidence` is at least 0.5.
        std::optional<double> estimated_prior_parameter;

        /// How pronounced the notch is, from 0 to 1. The notch depth d of a
        /// point is how far its log error falls below the straight line
        /// between its two neighbors; confidence is d / (d + 3 s) for the
        /// deepest notch, where s is the median notch depth of the other
        /// interior points, or 0.05 if that is larger. The first and last
        /// parameters have no two neighbors, so a prior at either end of the
        /// sweep cannot be found. A flat curve means no prior compression was
        /// detected, not that there was none.
        double confidence = 0.0;

        StageRecord record;

        [[nodiscard]] json::Value to_json() const;
    };

    struct RecompressionOptions
    {
        /// The codec to re-encode with. Its encode_image() rules apply at
        /// every point, with the quality parameter in RateControl::quality()
        /// units.
        ImageCodec codec = ImageCodec::Mjpeg;

        /// Quality parameters to sweep, in the codec's own units. At least
        /// three distinct values, in any order.
        std::vector<double> parameter_range;

        /// Metric used to measure the difference at each point. A point's
        /// error is the MSE for Psnr, and 1 - SSIM for Ssim.
        Metric metric = Metric::Psnr;

        /// The format and color to re-encode in. Unset, the frame's own. A
        /// frame in anything else is converted once, up front, and the
        /// conversion recorded; each point's error is measured against the
        /// converted frame, so the conversion's own loss does not count.
        std::optional<PixelFormat> pixel_format;
        std::optional<ColorSpec> color;

        /// Passed to the encoder at every point, e.g. {"cpu-used", "6"} for
        /// a faster AVIF sweep.
        std::map<std::string, std::string> encoder_options;
    };

    LOSSYLAB_REFLECT(RecompressionOptions, codec, parameter_range, metric, pixel_format, color, encoder_options);

    /// Sweeps a codec's quality parameter, measuring how much re-encoding
    /// changes the input at each setting.
    ///
    /// Experimental. The signal is strong for single-generation JPEG and WebP
    /// and weakens quickly with further processing; a resize after the original
    /// compression usually destroys it entirely. The record's params hold each
    /// point's notch depth.
    [[nodiscard]] RecompressionCurve recompression_curve(const Frame& frame,
                                                         const RecompressionOptions& options);
}
