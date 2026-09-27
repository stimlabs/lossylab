#pragma once

#include "lossylab/core/codec_id.hpp"
#include "lossylab/core/geometry.hpp"
#include "lossylab/core/json.hpp"
#include "lossylab/core/pixel_format.hpp"
#include "lossylab/core/reflect.hpp"
#include "lossylab/measure/measure_types.hpp"

#include <array>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lossylab
{
    /// How a decoder brought subsampled chroma up to full resolution before
    /// converting to RGB.
    enum class ChromaUpsampling
    {
        /// Each chroma sample repeated: swscale's unscaled YUV-to-RGB path,
        /// which is what `ffmpeg -i in.jpg out.png` uses, and libjpeg without
        /// fancy upsampling.
        Replicate,

        /// Linear interpolation between centered chroma samples, 3/4 and 1/4
        /// weights: libjpeg-turbo's fancy upsampling (Pillow, OpenCV, most
        /// browsers) and libwebp's.
        Triangle
    };

    std::string to_string(ChromaUpsampling upsampling);
    ChromaUpsampling chroma_upsampling_from_string(std::string_view name);

    /// For reflect::from_json().
    inline void from_string(const std::string_view name, ChromaUpsampling& value)
    {
        value = chroma_upsampling_from_string(name);
    }

    /// What kind of evidence a trace rests on.
    enum class TraceEvidence
    {
        /// The quantization tables a JPEG file's own header declares, for a
        /// frame that is that file's decode as stored.
        JpegHeader,

        /// DCT coefficients that sit on a JPEG quantization lattice.
        JpegQuantization,

        /// Chroma that is exactly what upsampling a subsampled chroma plane
        /// produces.
        ChromaSubsampling,

        /// A notch in a recompression curve.
        Recompression
    };

    std::string to_string(TraceEvidence evidence);
    TraceEvidence trace_evidence_from_string(std::string_view name);

    /// For reflect::from_json().
    inline void from_string(const std::string_view name, TraceEvidence& value)
    {
        value = trace_evidence_from_string(name);
    }

    /// A JPEG quantization table read off decoded pixels, or off a JPEG
    /// file's header.
    struct QuantizationEstimate
    {
        /// The quantization step of each DCT coefficient, in natural
        /// (row-major) order, or 0 where the pixels do not determine it: too
        /// few coefficients away from zero, or a step of 1, which leaves no
        /// lattice to see. A header determines every step, 1 included.
        std::array<int, 64> values{};

        /// How many of `values` are determined.
        int determined = 0;

        /// The libjpeg quality (1 to 100) whose standard table agrees best
        /// with the determined values, when any are.
        std::optional<int> ijg_quality;

        /// The fraction of the determined values that equal libjpeg's table
        /// at `ijg_quality`: 1 for a file libjpeg, libjpeg-turbo or anything
        /// built on them wrote, less for another encoder's tables, for which
        /// `ijg_quality` is only the nearest equivalent.
        double ijg_match = 0.0;

        /// How closely the coefficients sit on the lattice of the determined
        /// steps, averaged over them: 1 exactly on it, 0 for no lattice.
        /// Absent for a table read off a header.
        std::optional<double> lattice_score;

        [[nodiscard]] json::Value to_json() const;
    };

    LOSSYLAB_REFLECT(QuantizationEstimate, values, determined, ijg_quality, ijg_match, lattice_score);

    /// Whether a frame's 8x8 DCT coefficients sit on a JPEG quantization
    /// lattice, which a JPEG decode leaves behind through any lossless step
    /// that follows: a PNG or lossless WebP save, a conversion to RGB and
    /// back, a crop. Or, for a frame that is a JPEG file's decode as stored,
    /// what the file's header declares, with the fields only pixels can
    /// measure absent.
    struct JpegQuantizationEvidence
    {
        /// Whether the luma coefficients sit on a lattice: a grid score of at
        /// least 0.5, at least 0.2 above `runner_up_grid_score`, with at
        /// least three steps determined. Always true from a header.
        bool detected = false;

        /// Where the 8x8 block grid starts in this frame, 0 to 7 in each
        /// direction. Not 0 when the frame was cropped after compression by
        /// an amount that is not a multiple of 8.
        int grid_x = 0;
        int grid_y = 0;

        /// How well the coefficients fit a lattice at the grid offset found,
        /// from 0 to 1: the mean of the five best fits among the 63 AC
        /// coefficients, each less two standard deviations of its noise. And
        /// the same at the best offset that differs from it in both
        /// directions; offsets that share a row or a column with the true
        /// grid keep part of its lattice.
        ///
        /// Every offset is first screened on at most 256 blocks; both scores
        /// are then taken on at most 2048. When the screening singles out one
        /// offset (at least 0.3, and at least 0.2 above every offset that
        /// differs from it in both directions), only its three best offsets,
        /// offset (0, 0), the offsets sharing a row or column with its best
        /// one, and the three best that differ from the grid chosen in both
        /// directions, are scored again; otherwise every offset is. The
        /// evidence lists the 64 screening scores under `luma_grid_scores`.
        /// Both absent from a header.
        std::optional<double> grid_score;
        std::optional<double> runner_up_grid_score;

        /// Luma blocks the tables were estimated from, after leaving out any
        /// block with a sample clipped to 0 or 255, which breaks the lattice.
        /// Absent from a header.
        std::optional<int> blocks;

        QuantizationEstimate luma;

        /// Absent for a frame without chroma, or when no chroma layout shows
        /// a lattice, as at qualities whose chroma steps are 1 and 2.
        std::optional<QuantizationEstimate> chroma;

        /// The chroma layout the JPEG was coded in: the header's sampling
        /// factors, else the frame's own when it is still in the JPEG's YUV,
        /// else what ChromaSubsamplingEvidence found, under which `chroma` is
        /// estimated.
        std::optional<Subsampling> chroma_subsampling;

        /// The libjpeg quality whose luminance table agrees best with the
        /// luma estimate, as libjpeg scales both tables from one setting,
        /// with the chrominance table breaking ties when its own estimate
        /// matches a libjpeg table (at least 0.9 of it); and the fraction of
        /// the tables' determined values it matches. Each table's own best
        /// match is under `luma` and `chroma`.
        ///
        /// Qualities whose tables differ only where the pixels determine no
        /// step cannot be told apart; `ijg_quality_lowest` and
        /// `ijg_quality_highest` span those that fit equally well, and
        /// `ijg_quality` is the middle one. A span of more than one quality
        /// is common below about 60, where adjacent settings round most
        /// steps to the same value.
        std::optional<int> ijg_quality;
        std::optional<int> ijg_quality_lowest;
        std::optional<int> ijg_quality_highest;
        double ijg_match = 0.0;

        [[nodiscard]] json::Value to_json() const;
    };

    LOSSYLAB_REFLECT(JpegQuantizationEvidence, detected, grid_x, grid_y, grid_score, runner_up_grid_score, blocks,
                      luma, chroma, chroma_subsampling, ijg_quality, ijg_quality_lowest, ijg_quality_highest,
                      ijg_match);

    /// Whether full-resolution chroma was upsampled from subsampled chroma,
    /// which every 4:2:0 and 4:2:2 codec (JPEG, WebP, AVIF, video) leaves
    /// behind once decoded to RGB, whatever the quality.
    ///
    /// Per direction, the chroma is paired up, reduced to one sample per
    /// pair by undoing each ChromaUpsampling exactly, upsampled again, and
    /// compared with itself. Upsampled chroma comes back unchanged except
    /// for rounding, and only when paired up the way it was upsampled; any
    /// other chroma changes, and about equally at either pairing. Measured
    /// in 8-bit chroma code values, on the YCbCr of JPEG (BT.601, full
    /// range).
    struct ChromaSubsamplingEvidence
    {
        /// Yuv420, Yuv422 or Yuv440 when upsampling is found, Yuv444 when it
        /// is not; absent when there is no chroma to judge (an achromatic
        /// frame) or too little of it.
        std::optional<Subsampling> subsampling;

        /// How the subsampled chroma was upsampled, when it was.
        std::optional<ChromaUpsampling> upsampling;

        /// Per direction, for the upsampling that fits best: the RMS change
        /// at the better pairing (`residual`), its ratio to the change at the
        /// other pairing (`phase_ratio`), and its ratio to the other
        /// upsampling's change at its own better pairing
        /// (`upsampling_ratio`).
        ///
        /// A direction counts as subsampled when its phase ratio is at most
        /// 0.85 and one of these holds: its change is at most 0.25, the
        /// rounding floor of 8-bit RGB; its upsampling ratio is at most 0.45;
        /// or its phase ratio alone is at most 0.4. Chroma that is only
        /// blocky prefers a pairing too, but changes by more and fits both
        /// upsamplings about as well. The floor is higher for strongly
        /// colored content, which is what the two ratios cover.
        double horizontal_residual = 0.0;
        double horizontal_phase_ratio = 1.0;
        double horizontal_upsampling_ratio = 1.0;
        double vertical_residual = 0.0;
        double vertical_phase_ratio = 1.0;
        double vertical_upsampling_ratio = 1.0;

        /// From 0 to 1, how far past its thresholds the less certain
        /// direction is, on whichever side of them.
        double confidence = 0.0;

        [[nodiscard]] json::Value to_json() const;
    };

    LOSSYLAB_REFLECT(ChromaSubsamplingEvidence, subsampling, upsampling, horizontal_residual, horizontal_phase_ratio,
                      horizontal_upsampling_ratio, vertical_residual, vertical_phase_ratio, vertical_upsampling_ratio,
                      confidence);

    /// One earlier lossy compression, or a processing step typical of one,
    /// found in a frame.
    struct CompressionTrace
    {
        TraceEvidence evidence = TraceEvidence::JpegQuantization;

        /// The codec it points to: Mjpeg stands for JPEG in general. Absent
        /// for chroma subsampling, which many codecs share.
        std::optional<ImageCodec> codec;

        /// The quality it was compressed at, in `quality_scale`'s units.
        /// Absent for a JPEG whose tables are not libjpeg's (an
        /// `ijg_match` below 0.8), whose libjpeg quality would only be the
        /// nearest equivalent; JpegQuantizationEvidence still has it.
        std::optional<double> quality;
        std::string quality_scale;

        std::optional<Subsampling> subsampling;

        /// From 0 to 1. A JPEG quantization trace's is its grid score; a JPEG
        /// header trace's is 1; a recompression trace's is the coarse curve's
        /// confidence.
        double confidence = 0.0;

        [[nodiscard]] json::Value to_json() const;
    };

    LOSSYLAB_REFLECT(CompressionTrace, evidence, codec, quality, quality_scale, subsampling, confidence);

    struct CompressionHistoryOptions
    {
        /// Codecs to test by recompression, each with a coarse sweep of its
        /// whole quality scale and a fine one around the curve's notch. WebP
        /// by default. JPEG needs none: its tables are read directly. AVIF,
        /// JPEG XL and JPEG 2000 are accepted, but see recompression_curve()
        /// for how little their curves have been checked. JPEG 2000 sweeps
        /// nominal ratios 4 to 200, so a file at ratio 4 or less (all but
        /// lossless) is not found. A codec this build cannot
        /// both encode and decode is skipped, and listed in the evidence. A
        /// frame whose JPEG quantization is detected with a grid score above
        /// 0.8 is swept with MJPEG alone; the other codecs are listed in the
        /// evidence under `recompression.skipped_after_jpeg`. A frame whose
        /// chroma is found to be 4:4:4 is not swept with WebP, which codes
        /// 4:2:0 alone; it is listed under `recompression.skipped_for_chroma`.
        std::vector<ImageCodec> recompression_codecs{ImageCodec::WebP};

        /// The side of the centered square the recompression sweeps run on,
        /// since every point costs an encode; 0 for the whole frame. The
        /// square is placed on the 16-pixel grid, so block grids line up. A
        /// frame smaller than 128 x 128 still gets its curves, but no
        /// recompression trace: on so few blocks a notch is as likely noise.
        int recompression_crop = 1024;

        /// Confidence a trace needs to be listed.
        double min_confidence = 0.5;

        /// When the JPEG tables come from the file's header, also estimate
        /// them from the pixels, into the evidence's `jpeg_pixel_check`, with
        /// how well the two agree in `jpeg_pixel_agreement`.
        bool jpeg_pixel_check = false;
    };

    LOSSYLAB_REFLECT(CompressionHistoryOptions, recompression_codecs, recompression_crop, min_confidence,
                      jpeg_pixel_check);

    /// One recompression sweep compression_history() ran: the options it ran
    /// with, resolved, and what it measured.
    struct RecompressionSweep
    {
        RecompressionOptions options;
        RecompressionCurveEvidence curve;
    };

    LOSSYLAB_REFLECT(RecompressionSweep, options, curve);

    /// How many of the steps the pixels determine equal a header's.
    struct JpegTableAgreement
    {
        int determined = 0;
        int matching = 0;
    };

    LOSSYLAB_REFLECT(JpegTableAgreement, determined, matching);

    /// How well the pixels' JPEG tables agree with the header's.
    struct JpegPixelAgreement
    {
        bool detected = false;
        bool ijg_quality_equal = false;
        JpegTableAgreement luma;

        /// Absent unless both the pixels and the header have a chroma table.
        std::optional<JpegTableAgreement> chroma;
    };

    LOSSYLAB_REFLECT(JpegPixelAgreement, detected, ijg_quality_equal, luma, chroma);

    /// What became of the recompression codecs for this frame.
    struct RecompressionOutcome
    {
        /// The centered square the sweeps ran on; absent when they ran on the
        /// whole frame.
        std::optional<Rect> crop;

        /// Codecs this build cannot both encode and decode.
        std::vector<ImageCodec> skipped;

        /// Codecs not swept because the frame's JPEG quantization was found
        /// with a grid score above 0.8.
        std::vector<ImageCodec> skipped_after_jpeg;

        /// WebP, when the frame's chroma was found to be 4:4:4.
        std::vector<ImageCodec> skipped_for_chroma;

        /// Per codec name, the error its sweep failed with.
        std::map<std::string, std::string> errors;
    };

    LOSSYLAB_REFLECT(RecompressionOutcome, crop, skipped, skipped_after_jpeg, skipped_for_chroma, errors);

    /// What compression_history() found.
    struct CompressionHistoryEvidence
    {
        /// The pixel format the frame was analyzed in.
        std::string analyzed_as;

        /// "none" for a frame without chroma, "achromatic" for chroma that is
        /// neutral throughout, else the chroma layout analyzed.
        std::string chroma_layout;

        /// "header" when the JPEG tables come from the file's header, else
        /// "pixels", and why the header was not used.
        std::string jpeg_tables;
        std::optional<std::string> jpeg_header_unused;

        /// The 64 screening scores of the luma grid offsets, in raster order;
        /// empty when the pixels were not searched for a lattice.
        std::vector<double> luma_grid_scores;

        /// Every trace found with at least `min_confidence`, most confident
        /// first. Empty means none was found, not that there was none: a
        /// resize, a blur, added noise or a color change after the last
        /// compression erases most of these.
        std::vector<CompressionTrace> traces;

        /// From the JPEG file's header when the frame is that file's decode as
        /// stored, else from the pixels. Absent when the frame is too small
        /// for an 8x8 grid.
        std::optional<JpegQuantizationEvidence> jpeg;

        /// The pixels' estimate, when `jpeg` came from the header and the
        /// options asked for `jpeg_pixel_check`, and how well it agrees.
        std::optional<JpegQuantizationEvidence> jpeg_pixel_check;
        std::optional<JpegPixelAgreement> jpeg_pixel_agreement;

        /// Absent when the frame's chroma is not at full resolution, as in a
        /// frame still in a subsampled YUV format, whose layout says it.
        std::optional<ChromaSubsamplingEvidence> chroma;

        /// Per codec, the coarse sweep, then the fine one when there is one:
        /// when the coarse curve has a notch and at least `min_confidence`.
        std::vector<RecompressionSweep> recompression_curves;

        RecompressionOutcome recompression;
    };

    LOSSYLAB_REFLECT(CompressionHistoryEvidence, analyzed_as, chroma_layout, jpeg_tables, jpeg_header_unused,
                      luma_grid_scores, traces, jpeg, jpeg_pixel_check, jpeg_pixel_agreement, chroma,
                      recompression_curves, recompression);
}
