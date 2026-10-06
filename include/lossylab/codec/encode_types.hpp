#pragma once

#include "lossylab/convert/convert_types.hpp"
#include "lossylab/core/codec_id.hpp"
#include "lossylab/core/color_spec.hpp"
#include "lossylab/core/json.hpp"
#include "lossylab/core/kernel.hpp"
#include "lossylab/core/pixel_format.hpp"
#include "lossylab/core/rational.hpp"
#include "lossylab/core/reflect.hpp"
#include "lossylab/core/strict.hpp"
#include "lossylab/io/decode_image_types.hpp"
#include "lossylab/io/video_reader_types.hpp"

#include <cstdint>
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

        /// Image codecs' quality scale, in each encoder's own units: JPEG's
        /// IJG quality (an integer from 1 to 100, higher is better, the one
        /// probe() and compression_history() report), MJPEG's
        /// qscale (an integer from 1 to 31, lower is better), WebP's quality
        /// (0 to 100, higher is better; for lossless WebP an effort), JPEG
        /// XL's Butteraugli distance (0.01 to 15, lower is better), and for
        /// AVIF the crf of libaom-av1 (0 to 63) or libsvtav1 (1 to 63) or
        /// the quantizer of librav1e (0 to 255), lower is better. JPEG 2000
        /// takes a nominal compression ratio (FFmpeg's layer_rates, 1 to
        /// 1000, lower is better).
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

        /// Consecutive B-frames between references. Zero disables them. Only
        /// x264 and x265 take more: VP9 and AV1 encoders have no B-frames to
        /// configure, and SVT-AV1 is run with low-delay prediction.
        int b_frames = 0;

        /// Whether the encoder may insert keyframes at scene changes. Off,
        /// keyframes fall at exactly every `keyframe_interval` frames. On,
        /// x264 and x265 use their scene-cut detection, and libvpx, libaom
        /// and rav1e place keyframes as they see fit within the interval.
        /// SVT-AV1 never inserts them, so it refuses this.
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

    LOSSYLAB_REFLECT(GopStructure, keyframe_interval, b_frames, scene_change_detection, b_pyramid, closed_gop);

    struct EncodeVideoOptions
    {
        VideoCodec codec = VideoCodec::H264;
        EncoderBackend backend = EncoderBackend::Software;

        RateControl rate_control = RateControl::crf(23.0);
        GopStructure gop;

        /// Container to mux into, by FFmpeg muxer name ("mp4", "matroska",
        /// "webm", ...). Empty means the codec's elementary stream format,
        /// which is what a roundtrip wants since nothing needs to seek it:
        /// Annex B for H.264 and HEVC, IVF for VP9 (which has no bare
        /// format), and low-overhead OBUs for AV1.
        std::string container;

        /// Pixel format handed to the encoder. Must be one the encoder
        /// accepts. Frames in another format are converted to it under
        /// Strict::AllowRecorded and refused under Strict::Refuse.
        PixelFormat pixel_format;

        /// Color to encode in, written into the bitstream. Unset means the
        /// frames' own; frames in another color are converted, subject to
        /// `strict`, never relabeled. Needed when RGB frames are encoded as
        /// YUV, since the matrix is then a choice.
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

    LOSSYLAB_REFLECT(EncodeVideoOptions, codec, backend, rate_control, gop, container, pixel_format, color,
                      frame_rate, encoder_options, thread_count, strict);

    struct EncodeImageOptions
    {
        ImageCodec codec = ImageCodec::WebP;

        RateControl rate_control = RateControl::quality(75.0);

        /// As for EncodeVideoOptions. Lossy WebP takes yuv420p or yuva420p
        /// and lossless WebP bgra, the formats libwebp encodes without a
        /// conversion of its own; JPEG takes yuvj420p, yuvj422p, yuvj440p or
        /// yuvj444p, which libjpeg-turbo takes as they are.
        PixelFormat pixel_format;

        /// As for EncodeVideoOptions. A format that cannot signal color
        /// restricts it: JPEG must be full-range BT.601 with centered chroma,
        /// lossy WebP limited-range BT.601 with centered chroma.
        std::optional<ColorSpec> color;

        /// The kernel that shrinks the chroma planes when the frame has to be
        /// converted to `pixel_format` first, such as RGB to 4:2:0.
        KernelSpec chroma_down{Kernel::Area, {}};

        /// Encode losslessly. PNG requires it; WebP, JPEG XL and JPEG 2000
        /// (the reversible 5/3 wavelet) support it; JPEG, MJPEG and AVIF
        /// refuse it.
        /// PNG, lossless JPEG XL and lossless JPEG 2000 have no quality
        /// parameter, and ignore the rate control. Lossy JPEG 2000 takes a
        /// quality that is FFmpeg's nominal compression ratio (layer_rates),
        /// not the achieved one: rgb24 at 8 came out at 7 to 9 bits per pixel
        /// on test textures, gray at 8 at 1.
        bool lossless = false;

        std::map<std::string, std::string> encoder_options;

        int thread_count = 1;
        Strict strict = Strict::Refuse;
    };

    LOSSYLAB_REFLECT(EncodeImageOptions, codec, rate_control, pixel_format, color, chroma_down, lossless,
                      encoder_options, thread_count, strict);

    /// How the bytes from an encode should be decoded back.
    struct DecodeSpec
    {
        /// The conversion the decoded frames go through. Unset means the
        /// codec's native format, which preserves the chroma structure the
        /// encoder produced.
        std::optional<ConvertOptions> conversion;
        int thread_count = 1;

        /// Applies to the decode itself; the conversion has its own.
        Strict strict = Strict::Refuse;
    };

    LOSSYLAB_REFLECT(DecodeSpec, conversion, thread_count, strict);

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

    LOSSYLAB_REFLECT(EncodeTarget, kind, value, tolerance, max_iterations);

    /// One encode tried by encode_to_target().
    struct EncodeAttempt
    {
        double quality_parameter = 0.0;
        double achieved = 0.0;
    };

    LOSSYLAB_REFLECT(EncodeAttempt, quality_parameter, achieved);

    /// How encode_to_target() chose the quality its configuration holds.
    struct EncodeSearch
    {
        EncodeTarget target;

        /// Every encode tried, in order.
        std::vector<EncodeAttempt> attempts;

        /// False when the search ran out of iterations or hit a parameter
        /// bound. The bytes are still the best attempt, but a caller
        /// stratifying on severity needs to know the target was not met.
        bool converged = false;

        /// The quality parameter the search settled on, and what it achieved.
        double quality_parameter = 0.0;
        double achieved = 0.0;
    };

    LOSSYLAB_REFLECT(EncodeSearch, target, attempts, converged, quality_parameter, achieved);

    /// The settings the encoder ended up with, read back from it once it was
    /// opened: the options as translated, and FFmpeg's defaults for the rest.
    struct EncoderResolution
    {
        Rational time_base{1, 25};
        int thread_count = 0;
        bool bitexact = false;

        /// The fixed quantizer, for an encoder run at a constant qscale.
        std::optional<int> fixed_qscale;
        int qmin = 0;
        int qmax = 0;
        std::int64_t bit_rate = 0;
        std::int64_t max_rate = 0;
        int buffer_size = 0;
        int gop_size = 0;
        int keyint_min = 0;
        int max_b_frames = 0;
        bool closed_gop = false;
        bool global_header = false;

        /// The encoder's own options, by name, as it reports their values.
        std::map<std::string, std::string> options;
    };

    LOSSYLAB_REFLECT(EncoderResolution, time_base, thread_count, bitexact, fixed_qscale, qmin, qmax, bit_rate,
                     max_rate, buffer_size, gop_size, keyint_min, max_b_frames, closed_gop, global_header, options);

    /// What an image encode produced.
    struct EncodeImageEvidence
    {
        /// The muxer the bytes were written with; absent for a codec whose
        /// bytes are its bitstream.
        std::optional<std::string> container;

        /// The file extension the bytes belong under.
        std::string extension;

        /// The color the frame was encoded in.
        ColorSpec color;

        EncoderResolution resolved;

        /// Bits per pixel of the encoded bytes.
        double achieved_bpp = 0.0;

        /// Set by encode_to_target().
        std::optional<EncodeSearch> search;
    };

    LOSSYLAB_REFLECT(EncodeImageEvidence, container, extension, color, resolved, achieved_bpp, search);

    /// What a video encode produced.
    struct EncodeVideoEvidence
    {
        /// The muxer the bytes were written with.
        std::string container;

        /// The file extension the bytes belong under.
        std::string extension;

        int frame_count = 0;

        /// The color the frames were encoded in.
        ColorSpec color;

        EncoderResolution resolved;

        /// Bits per pixel of the encoded bytes, over all frames.
        double achieved_bpp = 0.0;

        /// Set by encode_to_target().
        std::optional<EncodeSearch> search;
    };

    LOSSYLAB_REFLECT(EncodeVideoEvidence, container, extension, frame_count, color, resolved, achieved_bpp, search);

    /// An image encode and the decode of its output.
    struct RoundtripImageConfiguration
    {
        EncodeImageOptions encode;
        DecodeImageOptions decode;
    };

    LOSSYLAB_REFLECT(RoundtripImageConfiguration, encode, decode);

    struct RoundtripImageEvidence
    {
        EncodeImageEvidence encode;
        DecodeImageEvidence decode;
    };

    LOSSYLAB_REFLECT(RoundtripImageEvidence, encode, decode);

    /// A video encode and the decode of its output.
    struct RoundtripVideoConfiguration
    {
        EncodeVideoOptions encode;
        DecodeVideoConfiguration decode;
    };

    LOSSYLAB_REFLECT(RoundtripVideoConfiguration, encode, decode);

    struct RoundtripVideoEvidence
    {
        EncodeVideoEvidence encode;
        DecodeVideoEvidence decode;
    };

    LOSSYLAB_REFLECT(RoundtripVideoEvidence, encode, decode);
}
