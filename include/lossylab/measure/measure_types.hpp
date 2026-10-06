#pragma once

#include "lossylab/core/codec_id.hpp"
#include "lossylab/core/color_spec.hpp"
#include "lossylab/core/geometry.hpp"
#include "lossylab/core/json.hpp"
#include "lossylab/core/pixel_format.hpp"
#include "lossylab/core/reflect.hpp"
#include "lossylab/core/statistics.hpp"
#include "lossylab/core/strict.hpp"

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lossylab
{
    /// A no-reference statistic computable from a frame alone.
    ///
    /// These are what an audit runs over a dataset it did not create, where
    /// there is no pristine original to compare against. Values are in the code
    /// values of the format the analyzer measured, which the evidence lists
    /// under `measured_as`.
    enum class Analyzer
    {
        /// FFmpeg's signalstats. Sets `signal_levels`: for luma, the two chroma
        /// planes and saturation, the minimum, 10th percentile, mean, 90th
        /// percentile and maximum. Also `hue_median`, `hue_mean`, the bit
        /// depth each plane actually uses (`luma_bit_depth`,
        /// `chroma_u_bit_depth`, `chroma_v_bit_depth`), and
        /// `outside_limited_range`, the fraction of pixels with any component
        /// outside 16-235 luma or 16-240 chroma (scaled to the bit depth).
        /// Reveals limited versus full range directly, and catches the levels
        /// mismatch that a mislabeled range leaves behind. Planar YUV only.
        SignalLevels,

        /// FFmpeg's blockdetect on the first plane: `blockiness`, the gradient
        /// energy on the strongest regular grid between 3 and 24 pixels,
        /// relative to the energy off it. About 1 for an image without block
        /// artifacts, rising with block-transform compression strength.
        /// 8-bit formats only.
        Blockiness,

        /// FFmpeg's blurdetect on the first plane: `blurriness`, the mean width
        /// in pixels of the edges found by a Canny detector. Grows with blur,
        /// and with resolution for the same content. Absent for a frame with no
        /// edges. 8-bit formats only.
        Blurriness,

        /// Noise level: `noise_sigma`, the standard deviation of the noise on
        /// the luma plane in 8-bit code values, whatever the bit depth. Tai and
        /// Yang's variant of Immerkaer's estimator: the response of a
        /// Laplacian-difference kernel, averaged over all pixels except the 10%
        /// with the strongest Sobel gradient, so that edges and texture inflate
        /// it less. Absent for a frame smaller than 3x3.
        Noise,

        /// FFmpeg's cropdetect: the content rectangle inside black bars, found
        /// from rows and columns whose mean luma is at most 24/255 of full
        /// scale. Sets `letterbox`: `content_rect`, `content_fraction` and the
        /// `bars` (sizes in pixels). A frame that is black throughout has an
        /// empty `content_rect`, no `bars` and a `content_fraction` of 0.
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

    /// For reflect::from_json().
    inline void from_string(const std::string_view name, Analyzer& value)
    {
        value = analyzer_from_string(name);
    }

    /// How the values of one channel spread over a frame, in code values.
    struct Levels
    {
        double minimum = 0.0;
        double percentile_10 = 0.0;
        double mean = 0.0;
        double percentile_90 = 0.0;
        double maximum = 0.0;
    };

    LOSSYLAB_REFLECT(Levels, minimum, percentile_10, mean, percentile_90, maximum);

    /// The result of Analyzer::SignalLevels for one frame.
    struct SignalLevels
    {
        Levels luma;
        Levels chroma_u;
        Levels chroma_v;
        Levels saturation;
        double hue_mean = 0.0;
        double hue_median = 0.0;

        /// The bit depth each plane actually uses: the format's depth less the
        /// low bits that are zero in every sample, as when 8-bit material is
        /// stored in 10 bits.
        int luma_bit_depth = 0;
        int chroma_u_bit_depth = 0;
        int chroma_v_bit_depth = 0;

        /// The fraction of pixels with any component outside the limited range.
        double outside_limited_range = 0.0;
    };

    LOSSYLAB_REFLECT(SignalLevels, luma, chroma_u, chroma_v, saturation, hue_mean, hue_median, luma_bit_depth,
                     chroma_u_bit_depth, chroma_v_bit_depth, outside_limited_range);

    /// The black bars on each side of a frame, in pixels.
    struct LetterboxBars
    {
        int top = 0;
        int bottom = 0;
        int left = 0;
        int right = 0;
    };

    LOSSYLAB_REFLECT(LetterboxBars, top, bottom, left, right);

    /// The result of Analyzer::Letterbox for one frame.
    struct Letterbox
    {
        /// Empty (zero width and height) when the frame is black throughout.
        Rect content_rect;
        double content_fraction = 0.0;

        /// Unset when the frame is black throughout, since no side has bars
        /// then.
        std::optional<LetterboxBars> bars;
    };

    LOSSYLAB_REFLECT(Letterbox, content_rect, content_fraction, bars);

    /// One frame's measurements. An analyzer that was not run leaves its
    /// result unset.
    struct FrameMeasurement
    {
        int index = 0;

        std::optional<SignalLevels> signal_levels;

        /// NaN for a frame without content.
        std::optional<double> blockiness;

        /// Unset for a frame with no edges, even when the analyzer ran.
        std::optional<double> blurriness;

        /// Unset for a frame smaller than 3x3, even when the analyzer ran.
        std::optional<double> noise_sigma;

        std::optional<Letterbox> letterbox;

        [[nodiscard]] json::Value to_json() const;
    };

    LOSSYLAB_REFLECT(FrameMeasurement, index, signal_levels, blockiness, blurriness, noise_sigma, letterbox);

    struct MeasureOptions
    {
        /// The analyzers to run, at least one. A repeated analyzer runs once.
        std::vector<Analyzer> analyzers;

        /// What happens when an analyzer does not accept the frames' pixel
        /// format. Under Refuse, measure() throws ConversionRefused naming the
        /// analyzer. Under AllowRecorded, the frames are converted to the
        /// nearest format the analyzer accepts, and the conversion is recorded.
        Strict strict = Strict::Refuse;
    };

    LOSSYLAB_REFLECT(MeasureOptions, analyzers, strict);

    /// What measure() found.
    struct MeasureEvidence
    {
        /// Per analyzer name, the pixel format it measured in; the values are
        /// in that format's code values.
        std::map<std::string, std::string> measured_as;

        /// One per frame, in order.
        std::vector<FrameMeasurement> frames;

        /// Each number in the frames' measurements, summarized across the
        /// frames that have it, keyed by its dotted path, e.g.
        /// "signal_levels.luma.mean" or "blockiness". `count` tells how many
        /// frames it covers. The frame `index` is not pooled. Empty for fewer
        /// than two frames, where the frame's own values are all there is.
        std::map<std::string, statistics::Summary> pooled;
    };

    LOSSYLAB_REFLECT(MeasureEvidence, measured_as, frames, pooled);

    struct CompareOptions
    {
        /// The metrics to compute, at least one. A repeated metric runs once.
        std::vector<Metric> metrics;

        /// What happens when a metric's filter does not accept the frames'
        /// pixel format (neither takes packed RGB). Under Refuse, compare()
        /// throws ConversionRefused naming the filter. Under AllowRecorded,
        /// both sides are converted to the nearest format it accepts, and the
        /// conversion is recorded.
        Strict strict = Strict::Refuse;
    };

    LOSSYLAB_REFLECT(CompareOptions, metrics, strict);

    /// What compare() found.
    struct CompareEvidence
    {
        /// Per metric name, the pixel format it measured in.
        std::map<std::string, std::string> measured_as;

        /// Per frame, keyed by metric name. PSNR sets "psnr" (over all planes,
        /// weighted by their sample counts), "mse" (in the code values of the
        /// format it measured), and "psnr_<c>" and "mse_<c>" for each component
        /// c of y, u, v or r, g, b, plus a for alpha. PSNR is infinite for
        /// identical frames, and serializes as null. SSIM sets "ssim" (the
        /// planes weighted as FFmpeg's ssim filter weights them), "ssim_db",
        /// and "ssim_<c>" for each component.
        std::vector<std::map<std::string, double>> frames;

        /// Pooled across frames: the mean, sample standard deviation (n - 1,
        /// NaN for a single frame), median, min and max of each value, as
        /// "<name>_mean", "<name>_std", "<name>_median", "<name>_min" and
        /// "<name>_max". The median is NaN when any frame's value is NaN.
        /// "psnr_mean" is the mean of per-frame PSNR, which is the convention;
        /// it is not the PSNR of the mean MSE, and the two disagree on clips
        /// with varying quality.
        std::map<std::string, double> pooled;
    };

    LOSSYLAB_REFLECT(CompareEvidence, measured_as, frames, pooled);

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

    LOSSYLAB_REFLECT(RecompressionPoint, quality_parameter, error, bits_per_pixel);

    /// Which planes a recompression point's error covers.
    enum class RecompressionPlanes
    {
        /// Every plane, weighted by its sample count.
        All,

        /// The luma plane alone. Most of a prior compression's trace is in
        /// luma, while the chroma error also carries whatever the chroma
        /// conversion into the codec's format failed to undo (a decoder's
        /// chroma upsampling, or the encoder's own RGB-to-YUV conversion).
        /// Needs a YUV or gray format to re-encode in.
        Luma
    };

    std::string to_string(RecompressionPlanes planes);
    RecompressionPlanes recompression_planes_from_string(std::string_view name);

    /// For reflect::from_json().
    inline void from_string(const std::string_view name, RecompressionPlanes& value)
    {
        value = recompression_planes_from_string(name);
    }

    struct RecompressionOptions
    {
        /// The codec to re-encode with: JPEG (libjpeg-turbo, at an IJG
        /// quality), MJPEG, WebP (lossy), AVIF, JPEG XL or JPEG 2000.
        /// Its encode_image() rules apply at every point, with the quality
        /// parameter in RateControl::quality() units.
        ImageCodec codec = ImageCodec::Mjpeg;

        /// Quality parameters to sweep, in the codec's own units. At least
        /// three distinct values, in any order.
        std::vector<double> parameter_range;

        /// Metric used to measure the difference at each point. A point's
        /// error is the MSE for Psnr, and 1 - SSIM for Ssim, over `planes`.
        Metric metric = Metric::Psnr;

        // TODO: one or two lines of documentation
        RecompressionPlanes planes = RecompressionPlanes::All;

        /// The format to re-encode in. Unset, the codec's own format nearest
        /// the frame's: for MJPEG yuvj444p, yuvj422p or yuvj420p by the
        /// frame's chroma subsampling (yuvj420p for RGB, yuvj444p with
        /// neutral chroma for gray), the same for JPEG but with yuvj440p for
        /// 4:4:0, for WebP yuv420p, for AVIF 8-bit
        /// yuv444p, yuv422p or yuv420p (yuv420p for RGB, gray for gray), and
        /// for JPEG XL and JPEG 2000 rgb24, or gray for gray. None of these has alpha:
        /// alpha is dropped and only the color planes are measured.
        std::optional<PixelFormat> pixel_format;

        /// The color to re-encode in. Unset, the frame's own, with whatever
        /// the codec fixes put in place: BT.601 full range with centered
        /// chroma for JPEG, BT.601 limited range with centered chroma for
        /// lossy WebP, and for an RGB frame encoded as AVIF BT.601 full range
        /// with left chroma, libavif's defaults.
        ///
        /// Either way, a frame whose own color leaves fields unspecified has
        /// them filled first from the codec's implied color (centered chroma
        /// for a decoded WebP, say), on sRGB primaries and transfer, and the
        /// fill is recorded. A frame in another format or color is converted
        /// once, up front, and the conversion recorded; each point's error is
        /// measured against the converted frame, so the conversion's own loss
        /// does not count.
        std::optional<ColorSpec> color;

        /// Passed to the encoder at every point, e.g. {"cpu-used", "6"} for
        /// a faster AVIF sweep.
        std::map<std::string, std::string> encoder_options;
    };

    LOSSYLAB_REFLECT(RecompressionOptions, codec, parameter_range, metric, planes, pixel_format, color,
                      encoder_options);

    /// What recompression_curve() measured.
    struct RecompressionCurveEvidence
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

        /// Each point's notch depth, 0 for the first and last, and the `s`
        /// that confidence divides by.
        std::vector<double> notch_depths;
        double noise_scale = 0.0;

        /// "dropped" when the frame had alpha the re-encoding format has not,
        /// else "kept".
        std::string alpha;
    };

    LOSSYLAB_REFLECT(RecompressionCurveEvidence, points, estimated_prior_parameter, confidence, notch_depths,
                      noise_scale, alpha);
}
