#pragma once

#include "lossylab/core/codec_id.hpp"
#include "lossylab/core/frame.hpp"
#include "lossylab/core/json.hpp"
#include "lossylab/core/result.hpp"
#include "lossylab/core/strict.hpp"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace lossylab
{
    /// How an encoder decides bit allocation.
    ///
    /// Stated explicitly and never defaulted: the same codec at the same
    /// nominal quality produces very different artifacts under CRF than under a
    /// fixed bitrate, and a difference in rate control between two classes of
    /// data would be a trivially learnable giveaway.
    class RateControl
    {
    public:
        /// Constant quality. The usual choice for augmentation, since severity
        /// stays put as content changes.
        static RateControl crf(double value);

        /// Fixed quantizer, no rate adaptation at all.
        static RateControl constant_qp(int qp);

        /// Average bitrate in bits per second.
        static RateControl bitrate(std::int64_t bits_per_second);

        /// Constrained bitrate with a video buffering verifier: the mode
        /// broadcast and streaming platforms actually use, and the one that
        /// produces the characteristic quality swings after a scene change.
        static RateControl constrained(std::int64_t bits_per_second,
                                       std::int64_t max_rate,
                                       std::int64_t buffer_size);

        /// Image codecs' quality scale, whose meaning is codec-specific.
        static RateControl quality(double value);

        enum class Mode
        {
            Crf,
            ConstantQp,
            Bitrate,
            Constrained,
            Quality
        };

        [[nodiscard]] Mode mode() const noexcept { return m_mode; }
        [[nodiscard]] double value() const noexcept { return m_value; }
        [[nodiscard]] std::int64_t rate() const noexcept { return m_rate; }
        [[nodiscard]] std::int64_t max_rate() const noexcept { return m_max_rate; }
        [[nodiscard]] std::int64_t buffer_size() const noexcept { return m_buffer_size; }

        /// The encoder parameter that carries quality for this mode, as a
        /// number: CRF value, QP, or quality scale. Absent for bitrate modes,
        /// where quality is not a parameter at all.
        [[nodiscard]] std::optional<double> quality_parameter() const noexcept;

        /// A copy with the quality parameter replaced. The operation
        /// `encode_to_target` searches over.
        [[nodiscard]] RateControl with_quality_parameter(double value) const;

        [[nodiscard]] std::string describe() const;
        [[nodiscard]] json::Value to_json() const;
        static RateControl from_json(const json::Value& value);

    private:
        Mode m_mode = Mode::Crf;
        double m_value = 23.0;
        std::int64_t m_rate = 0;
        std::int64_t m_max_rate = 0;
        std::int64_t m_buffer_size = 0;
    };

    /// Group-of-pictures structure.
    ///
    /// Determines which frames are I, P or B, which is the single biggest
    /// driver of per-frame quality in a clip and therefore of how training
    /// frames should be stratified.
    struct GopStructure
    {
        /// Maximum frames between keyframes.
        int keyframe_interval = 250;

        /// Consecutive B-frames between references. Zero disables them.
        int b_frames = 0;

        /// Whether the encoder may insert keyframes at scene changes.
        bool scene_change_detection = true;

        /// Whether B-frames may themselves be references.
        bool b_pyramid = false;

        /// Closed GOPs do not reference across a keyframe, which is what makes
        /// a clip cuttable at keyframes without artifacts.
        bool closed_gop = false;

        [[nodiscard]] json::Value to_json() const;
        static GopStructure from_json(const json::Value& value);

        /// Every frame is a keyframe: the intra-only configuration, which is
        /// what still-image codecs and some platform paths use.
        static GopStructure intra_only() noexcept;
    };

    struct EncodeVideoOptions
    {
        VideoCodec codec = VideoCodec::H264;
        EncoderBackend backend = EncoderBackend::Software;

        RateControl rate_control = RateControl::crf(23.0);
        GopStructure gop;

        /// Container to mux into. Empty means a raw elementary stream, which
        /// is what a roundtrip wants since nothing needs to seek it.
        std::string container;

        /// Pixel format handed to the encoder. Must be one the encoder accepts;
        /// no conversion is inserted to make it fit.
        PixelFormat pixel_format;

        /// Color written into the bitstream. Must match the frames' color, or
        /// the encode is relabeling data without converting it.
        std::optional<ColorSpec> color;

        Rational frame_rate{25, 1};

        /// Encoder-specific options passed through verbatim, e.g. {"preset",
        /// "slow"}, {"tune", "film"}. Validated against the encoder's schema
        /// before the encode starts.
        std::map<std::string, std::string> encoder_options;

        /// Pinned, because several encoders produce different output at
        /// different thread counts. Reproducibility requires fixing it.
        int thread_count = 1;

        /// Refuses any conversion the encoder would otherwise insert to make
        /// the frames fit what it accepts.
        Strict strict = Strict::Refuse;
    };

    /// Encodes frames to a byte stream.
    ///
    /// The record carries per-frame type, size and quantizer, the achieved bits
    /// per pixel, the fully resolved encoder settings, and the block grid the
    /// codec imposed. Hardware backends are marked non-reproducible.
    [[nodiscard]] EncodedResult encode_video(const std::vector<Frame>& frames,
                                             const EncodeVideoOptions& options);

    struct EncodeImageOptions
    {
        ImageCodec codec = ImageCodec::WebP;

        RateControl rate_control = RateControl::quality(75.0);

        PixelFormat pixel_format;
        std::optional<ColorSpec> color;

        /// Encode losslessly where the codec supports it.
        bool lossless = false;

        std::map<std::string, std::string> encoder_options;

        int thread_count = 1;
        Strict strict = Strict::Refuse;
    };

    /// Encodes a single image.
    ///
    /// Covers the platform delivery formats. FFmpeg's MJPEG is available as a
    /// second JPEG implementation when encoder diversity is the point; PIL
    /// remains the primary JPEG path by design.
    [[nodiscard]] EncodedResult encode_image(const Frame& frame,
                                             const EncodeImageOptions& options);

    /// How the bytes from an encode should be decoded back.
    struct DecodeSpec
    {
        /// Format to decode into. Unset means the codec's native format, which
        /// preserves the chroma structure the encoder produced.
        std::optional<PixelFormat> pixel_format;
        std::optional<ColorSpec> color;
        int thread_count = 1;
        Strict strict = Strict::Refuse;
    };

    /// Encode then decode, entirely in memory.
    ///
    /// The common augmentation case: no files, and no trip out to NumPy between
    /// the two halves. The record covers both, so a frame's compression history
    /// is one entry rather than two that have to be correlated afterwards.
    [[nodiscard]] FramesResult roundtrip(const std::vector<Frame>& frames,
                                         const EncodeVideoOptions& encode_spec,
                                         const DecodeSpec& decode_spec = {});

    /// Single-image equivalent.
    [[nodiscard]] FrameResult roundtrip(const Frame& frame,
                                        const EncodeImageOptions& encode_spec,
                                        const DecodeSpec& decode_spec = {});

    /// What an encode should be driven to hit.
    struct EncodeTarget
    {
        enum class Kind
        {
            BitsPerPixel,
            Psnr,
            Ssim,
            Vmaf
        };

        Kind kind = Kind::BitsPerPixel;
        double value = 0.5;

        /// Acceptable distance from `value`. The search stops inside this band.
        double tolerance = 0.02;

        /// Cap on encode attempts, since each one costs a full encode.
        int max_iterations = 8;

        [[nodiscard]] std::string describe() const;
        [[nodiscard]] json::Value to_json() const;
        static EncodeTarget from_json(const json::Value& value);
    };

    /// The outcome of a search, including whether it converged.
    struct EncodeToTargetResult
    {
        std::vector<std::uint8_t> bytes;
        StageRecord record;

        /// The quality parameter the search settled on.
        double quality_parameter = 0.0;

        double achieved = 0.0;
        int iterations = 0;

        /// False when the search ran out of iterations or hit a parameter
        /// bound. The bytes are still the best attempt, but a caller
        /// stratifying on severity needs to know the target was not met.
        bool converged = false;
    };

    /// Searches a codec's quality parameter to hit a target.
    ///
    /// Codec parameters are not comparable to each other: CRF 23 in x264 is not
    /// CRF 23 in x265, and neither means anything to WebP. Targeting bits per
    /// pixel or a quality metric instead gives one unit that severity can be
    /// sampled in across every codec, which is what makes two classes of data
    /// comparable on compression strength.
    [[nodiscard]] EncodeToTargetResult encode_to_target(const std::vector<Frame>& frames,
                                                        const EncodeVideoOptions& options,
                                                        const EncodeTarget& target);

    [[nodiscard]] EncodeToTargetResult encode_to_target(const Frame& frame,
                                                        const EncodeImageOptions& options,
                                                        const EncodeTarget& target);
}
