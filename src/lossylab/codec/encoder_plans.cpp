#include "encoder_plans.hpp"

#include "lossylab/core/error.hpp"
#include "lossylab/env/build_info.hpp"

extern "C" {
#include <libavformat/avformat.h>
}

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <iomanip>
#include <sstream>

namespace lossylab::detail
{
    namespace
    {
        std::string number_text(const double value)
        {
            std::ostringstream text;
            text << std::setprecision(12) << value;
            return text.str();
        }

        /// The rate control's value, checked against the encoder's range and
        /// recorded in the plan.
        double scaled_value(const RateControl& rate_control, const std::string& encoder_name,
                            const std::optional<QualityRange>& range, EncoderPlan& plan)
        {
            const double value = rate_control.value();
            if ((range->integral && value != std::floor(value)) || value < range->minimum || value > range->maximum)
            {
                throw ConfigError("encoder '" + encoder_name + "' takes " + range->description + ", not " +
                                  number_text(value));
            }
            plan.quality_range = range;
            return value;
        }

        void require_mode(const RateControl& rate_control, const std::initializer_list<RateControl::Mode> modes,
                          const std::string& encoder_name)
        {
            for (const RateControl::Mode mode : modes)
            {
                if (rate_control.mode() == mode)
                {
                    return;
                }
            }
            throw ConfigError("encoder '" + encoder_name + "' does not support rate control '" +
                              rate_control.describe() + "'");
        }

        bool is_bt601(const ColorMatrix matrix)
        {
            return matrix == ColorMatrix::Bt470bg || matrix == ColorMatrix::Smpte170m;
        }

        bool has_subsampled_chroma(const PixelFormat& pixel_format)
        {
            const Subsampling subsampling = pixel_format.subsampling();
            return subsampling != Subsampling::Yuv444 && subsampling != Subsampling::Rgb &&
                   subsampling != Subsampling::Gray;
        }

        /// What either JPEG encoder can carry: lossy, full range, BT.601 and
        /// centered chroma, none of which JPEG signals.
        void require_jpeg(const EncodeImageOptions& options, const PixelFormat& pixel_format, const ColorSpec& color)
        {
            if (options.lossless)
            {
                throw ConfigError("encode_image() has no lossless JPEG");
            }
            if (color.range != ColorRange::Full)
            {
                throw ConfigError("JPEG stores full-range samples; convert to full range first");
            }
            if (!is_bt601(color.matrix))
            {
                throw ConfigError("JPEG implies the BT.601 matrix, and cannot signal " + color.describe());
            }
            if (has_subsampled_chroma(pixel_format) && color.chroma_location != ChromaLocation::Center)
            {
                throw ConfigError("JPEG implies centered chroma, and cannot signal " + color.describe());
            }
        }

        bool is_srgb(const ColorSpec& color)
        {
            return color.primaries == ColorPrimaries::Bt709 && color.transfer == TransferCharacteristic::Srgb;
        }

        /// What FFmpeg's JP2 header says of the samples, which is all it
        /// says: sRGB for RGB, and sYCC for YUV, which is BT.601 at full range
        /// on sRGB primaries and transfer, with chroma at the top left, where
        /// the reference grid places a subsampled component.
        void require_jp2_color(const PixelFormat& pixel_format, const ColorSpec& color)
        {
            if (pixel_format.is_gray())
            {
                return;
            }
            const bool rgb = pixel_format.is_rgb();
            const bool chroma_fits = rgb || !has_subsampled_chroma(pixel_format) ||
                                     color.chroma_location == ChromaLocation::TopLeft;
            if (!is_srgb(color) || color.range != ColorRange::Full || (!rgb && !is_bt601(color.matrix)) ||
                !chroma_fits)
            {
                throw ConfigError(std::string("JPEG 2000 labels ") +
                                  (rgb ? "RGB as sRGB"
                                       : "YUV as sYCC: BT.601, full range, sRGB primaries and transfer, top-left "
                                         "chroma") +
                                  ", and cannot signal " + color.describe());
            }
        }

        /// Adds the caller's encoder options, which may not override one the
        /// library derived from the rate control or GOP structure.
        void add_caller_options(EncoderPlan& plan, const std::map<std::string, std::string>& caller_options)
        {
            for (const auto& [name, value] : caller_options)
            {
                if (!plan.setup.options.try_emplace(name, value).second)
                {
                    throw ConfigError("option '" + name + "' of encoder '" + plan.setup.encoder_name +
                                      "' is set from the rate control, GOP structure or lossless flag; set it "
                                      "there instead");
                }
            }
        }

        /// Sets the bitrate fields for the two bitrate modes.
        void apply_bitrate(const RateControl& rate_control, EncoderSetup& setup)
        {
            if (rate_control.mode() == RateControl::Mode::Bitrate ||
                rate_control.mode() == RateControl::Mode::Constrained)
            {
                setup.bit_rate = rate_control.rate();
            }
            if (rate_control.mode() == RateControl::Mode::Constrained)
            {
                setup.max_rate = rate_control.max_rate();
                setup.buffer_size = rate_control.buffer_size();
            }
        }

        void plan_avif(const std::string& encoder_name, const RateControl& rate_control, EncoderPlan& plan)
        {
            require_mode(rate_control, {RateControl::Mode::Quality}, encoder_name);
            const std::optional<QualityRange> range = image_quality_range(ImageCodec::Avif, false, encoder_name);
            if (!range.has_value())
            {
                throw NotImplemented("encode_image() with " + encoder_name);
            }
            const std::string value = number_text(scaled_value(rate_control, encoder_name, range, plan));
            if (encoder_name == "libaom-av1")
            {
                plan.setup.options["crf"] = value;
                plan.setup.options["still-picture"] = "1";
            }
            else if (encoder_name == "libsvtav1")
            {
                plan.setup.options["crf"] = value;
                plan.reports_qp = true;
            }
            else
            {
                plan.setup.options["qp"] = value;
            }
            plan.muxer = "avif";
            plan.extension = "avif";
        }
    }

    std::optional<QualityRange> image_quality_range(const ImageCodec codec, const bool lossless,
                                                    const std::string& encoder_name)
    {
        switch (codec)
        {
        case ImageCodec::Png:
        case ImageCodec::Heif:
            return std::nullopt;

        case ImageCodec::Jpeg:
            return QualityRange{1, 100, true, true, "an IJG quality, an integer from 1 to 100, higher is better"};

        case ImageCodec::Mjpeg:
            return QualityRange{1, 31, true, false, "qscale, an integer from 1 to 31, lower is better"};

        case ImageCodec::WebP:
            if (lossless)
            {
                return QualityRange{0, 100, false, false, "an effort from 0 to 100, higher is slower and smaller"};
            }
            return QualityRange{0, 100, false, true, "a quality from 0 to 100, higher is better"};

        case ImageCodec::Jxl:
            if (lossless)
            {
                return std::nullopt;
            }
            return QualityRange{0.01, 15, false, false, "a Butteraugli distance from 0.01 to 15, lower is better"};

        case ImageCodec::Avif:
            if (lossless)
            {
                return std::nullopt;
            }
            if (encoder_name == "libaom-av1")
            {
                return QualityRange{0, 63, true, false, "crf, an integer from 0 to 63, lower is better"};
            }
            if (encoder_name == "libsvtav1")
            {
                return QualityRange{1, 63, true, false, "crf, an integer from 1 to 63, lower is better"};
            }
            if (encoder_name == "librav1e")
            {
                return QualityRange{0, 255, true, false, "a quantizer, an integer from 0 to 255, lower is better"};
            }
            return std::nullopt;

        case ImageCodec::Jpeg2000:
            if (lossless)
            {
                return std::nullopt;
            }
            return QualityRange{1, 1000, true, false,
                                "a nominal compression ratio (FFmpeg's layer_rates), an integer from 1 to 1000, "
                                "lower is better"};
        }
        return std::nullopt;
    }

    std::optional<QualityRange> video_quality_range(const std::string& encoder_name, const RateControl::Mode mode,
                                                    const int bit_depth)
    {
        const bool crf = mode == RateControl::Mode::Crf;
        if (!crf && mode != RateControl::Mode::ConstantQp)
        {
            return std::nullopt;
        }

        if (encoder_name == "libx264" || encoder_name == "libx265")
        {
            // The QP range grows by 6 per bit above 8.
            const double maximum = 51.0 + 6.0 * std::max(0, bit_depth - 8);
            if (crf)
            {
                return QualityRange{0, maximum, false, false,
                                    "crf from 0 to " + number_text(maximum) + ", lower is better"};
            }
            return QualityRange{0, maximum, true, false,
                                "a QP, an integer from 0 to " + number_text(maximum) + ", lower is better"};
        }
        if (encoder_name == "libvpx-vp9" || encoder_name == "libaom-av1")
        {
            return QualityRange{0, 63, true, false,
                                crf ? "crf, an integer from 0 to 63, lower is better"
                                    : "a quantizer, an integer from 0 to 63, lower is better"};
        }
        if (encoder_name == "libsvtav1")
        {
            return QualityRange{1, 63, true, false,
                                std::string(crf ? "crf" : "a QP") + ", an integer from 1 to 63, lower is better"};
        }
        if (encoder_name == "librav1e" && !crf)
        {
            return QualityRange{0, 255, true, false, "a quantizer, an integer from 0 to 255, lower is better"};
        }
        return std::nullopt;
    }

    EncoderPlan plan_image_encode(const std::string& encoder_name, const EncodeImageOptions& options,
                                  const PixelFormat& pixel_format, const ColorSpec& color,
                                  const bool has_icc_profile)
    {
        EncoderPlan plan;
        plan.setup.encoder_name = encoder_name;
        const RateControl& rate_control = options.rate_control;

        switch (options.codec)
        {
        case ImageCodec::Png:
            if (!options.lossless)
            {
                throw ConfigError("PNG is lossless only; set lossless");
            }
            plan.extension = "png";
            break;

        case ImageCodec::Jpeg:
        {
            require_jpeg(options, pixel_format, color);
            require_mode(rate_control, {RateControl::Mode::Quality}, encoder_name);
            const auto range = image_quality_range(options.codec, options.lossless, encoder_name);
            plan.setup.ijg_quality = static_cast<int>(scaled_value(rate_control, encoder_name, range, plan));
            plan.block_grid = BlockGrid::jpeg_mcu(pixel_format.log2_chroma_width(), pixel_format.log2_chroma_height());
            plan.extension = "jpg";
            break;
        }

        case ImageCodec::Mjpeg:
        {
            require_jpeg(options, pixel_format, color);
            require_mode(rate_control, {RateControl::Mode::Quality}, encoder_name);
            const auto range = image_quality_range(options.codec, options.lossless, encoder_name);
            const int qscale = static_cast<int>(scaled_value(rate_control, encoder_name, range, plan));
            plan.setup.fixed_qscale = qscale;
            plan.setup.qmin = qscale;
            plan.setup.qmax = qscale;
            plan.block_grid = BlockGrid::jpeg_mcu(pixel_format.log2_chroma_width(), pixel_format.log2_chroma_height());
            plan.reports_qp = true;
            plan.extension = "jpg";
            break;
        }

        case ImageCodec::WebP:
        {
            require_mode(rate_control, {RateControl::Mode::Quality}, encoder_name);
            if (options.lossless)
            {
                if (pixel_format.name() != "bgra")
                {
                    throw ConfigError("lossless WebP takes bgra; libwebp would convert " + pixel_format.name() +
                                      " itself");
                }
                const auto range = image_quality_range(options.codec, options.lossless, encoder_name);
                plan.setup.options["quality"] = number_text(scaled_value(rate_control, encoder_name, range, plan));
                plan.setup.options["lossless"] = "1";
            }
            else
            {
                if (pixel_format.name() != "yuv420p" && pixel_format.name() != "yuva420p")
                {
                    throw ConfigError("lossy WebP takes yuv420p or yuva420p; libwebp would convert " +
                                      pixel_format.name() + " itself");
                }
                if (!is_bt601(color.matrix) || color.range != ColorRange::Limited ||
                    color.chroma_location != ChromaLocation::Center)
                {
                    throw ConfigError("lossy WebP implies limited-range BT.601 with centered chroma, and cannot "
                                      "signal " +
                                      color.describe());
                }
                const auto range = image_quality_range(options.codec, options.lossless, encoder_name);
                plan.setup.options["quality"] = number_text(scaled_value(rate_control, encoder_name, range, plan));
                plan.setup.options["lossless"] = "0";
                plan.block_grid = BlockGrid::for_kind(BlockGridKind::Macroblock16);
            }
            plan.extension = "webp";
            break;
        }

        case ImageCodec::Jxl:
            if (options.lossless)
            {
                plan.setup.options["distance"] = "0";
            }
            else
            {
                require_mode(rate_control, {RateControl::Mode::Quality}, encoder_name);
                const auto range = image_quality_range(options.codec, options.lossless, encoder_name);
                plan.setup.options["distance"] = number_text(scaled_value(rate_control, encoder_name, range, plan));

                // Every variable-size transform starts on the 8x8 grid.
                plan.block_grid = BlockGrid::for_kind(BlockGridKind::Dct8);
            }
            plan.extension = "jxl";
            break;

        case ImageCodec::Avif:
            if (options.lossless)
            {
                throw ConfigError("encode_image() has no lossless AVIF");
            }
            // FFmpeg's libaom wrapper labels RGB as sRGB, whatever it is.
            if (pixel_format.is_rgb() && !has_icc_profile && !is_srgb(color))
            {
                throw ConfigError("encoder '" + encoder_name + "' labels RGB as sRGB, and cannot signal " +
                                  color.describe() + " without an ICC profile");
            }
            plan_avif(encoder_name, rate_control, plan);
            plan.signals_chroma_location = true;
            break;

        case ImageCodec::Jpeg2000:
            require_jp2_color(pixel_format, color);
            if (options.lossless)
            {
                plan.setup.options["pred"] = "dwt53";
            }
            else
            {
                require_mode(rate_control, {RateControl::Mode::Quality}, encoder_name);
                const auto range = image_quality_range(options.codec, options.lossless, encoder_name);
                plan.setup.options["layer_rates"] = number_text(scaled_value(rate_control, encoder_name, range, plan));
            }
            plan.extension = "jp2";
            break;

        case ImageCodec::Heif:
            throw UnsupportedCapability("image muxer", "heif", build_info().identity_hash);
        }

        add_caller_options(plan, options.encoder_options);
        return plan;
    }

    namespace
    {
        void refuse_b_frames(const GopStructure& gop, const std::string& encoder_name)
        {
            if (gop.b_frames != 0 || gop.b_pyramid)
            {
                throw ConfigError("encoder '" + encoder_name +
                                  "' has no B-frames to configure; set b_frames to 0 and b_pyramid to false");
            }
        }

        std::string default_muxer(const VideoCodec codec)
        {
            switch (codec)
            {
            case VideoCodec::H264: return "h264";
            case VideoCodec::Hevc: return "hevc";
            case VideoCodec::Vp9: return "ivf";
            case VideoCodec::Av1: return "obu";
            }
            return "";
        }
    }

    EncoderPlan plan_video_encode(const std::string& encoder_name, const EncodeVideoOptions& options,
                                  const PixelFormat& pixel_format)
    {
        const RateControl& rate_control = options.rate_control;
        const GopStructure& gop = options.gop;
        if (gop.keyframe_interval < 1)
        {
            throw ConfigError("the keyframe interval must be at least 1");
        }
        if (gop.b_frames < 0)
        {
            throw ConfigError("b_frames must not be negative");
        }
        if (gop.b_pyramid && gop.b_frames < 2)
        {
            throw ConfigError("b_pyramid needs at least two consecutive B-frames");
        }
        if (options.frame_rate.num <= 0 || options.frame_rate.den <= 0)
        {
            throw ConfigError("the frame rate must be positive");
        }

        EncoderPlan plan;
        EncoderSetup& setup = plan.setup;
        setup.encoder_name = encoder_name;
        setup.gop_size = gop.keyframe_interval;
        setup.closed_gop = gop.closed_gop;
        apply_bitrate(rate_control, setup);

        const RateControl::Mode mode = rate_control.mode();
        const std::optional<QualityRange> range = video_quality_range(encoder_name, mode, pixel_format.bit_depth());

        if (encoder_name == "libx264" || encoder_name == "libx265")
        {
            require_mode(rate_control,
                         {RateControl::Mode::Crf, RateControl::Mode::ConstantQp, RateControl::Mode::Bitrate,
                          RateControl::Mode::Constrained},
                         encoder_name);
            if (mode == RateControl::Mode::Crf)
            {
                setup.options["crf"] = number_text(scaled_value(rate_control, encoder_name, range, plan));
            }
            else if (mode == RateControl::Mode::ConstantQp)
            {
                setup.options["qp"] = number_text(scaled_value(rate_control, encoder_name, range, plan));
            }
            setup.max_b_frames = gop.b_frames;
            plan.reports_qp = true;
            if (encoder_name == "libx264")
            {
                setup.options["b-pyramid"] = gop.b_pyramid ? "normal" : "none";
                setup.options["sc_threshold"] = gop.scene_change_detection ? "40" : "0";
                plan.block_grid = BlockGrid::for_kind(BlockGridKind::Macroblock16);
            }
            else
            {
                // x265 prints its info lines to stderr itself rather than
                // through FFmpeg's log.
                std::string parameters = std::string("log-level=warning:b-pyramid=") + (gop.b_pyramid ? "1" : "0");
                if (!gop.scene_change_detection)
                {
                    parameters += ":scenecut=0";
                }
                setup.options["x265-params"] = parameters;

                // CTUs are 32 or 64 pixels depending on the preset, so block
                // edges fall on every multiple of 64 either way.
                plan.block_grid = BlockGrid::for_kind(BlockGridKind::Ctu64);
            }
        }
        else if (encoder_name == "libvpx-vp9" || encoder_name == "libaom-av1")
        {
            require_mode(rate_control,
                         {RateControl::Mode::Crf, RateControl::Mode::ConstantQp, RateControl::Mode::Bitrate,
                          RateControl::Mode::Constrained},
                         encoder_name);
            refuse_b_frames(gop, encoder_name);
            if (mode == RateControl::Mode::Crf)
            {
                setup.options["crf"] = number_text(scaled_value(rate_control, encoder_name, range, plan));
            }
            else if (mode == RateControl::Mode::ConstantQp)
            {
                const int quantizer = static_cast<int>(scaled_value(rate_control, encoder_name, range, plan));
                setup.options["crf"] = std::to_string(quantizer);
                setup.qmin = quantizer;
                setup.qmax = quantizer;
            }
            if (!gop.scene_change_detection)
            {
                setup.keyint_min = gop.keyframe_interval;
            }
            if (encoder_name == "libvpx-vp9")
            {
                plan.block_grid = BlockGrid::for_kind(BlockGridKind::Ctu64);
                plan.reports_qp = true;
            }
        }
        else if (encoder_name == "libsvtav1")
        {
            require_mode(rate_control,
                         {RateControl::Mode::Crf, RateControl::Mode::ConstantQp, RateControl::Mode::Bitrate,
                          RateControl::Mode::Constrained},
                         encoder_name);
            refuse_b_frames(gop, encoder_name);
            if (gop.scene_change_detection)
            {
                throw ConfigError("encoder 'libsvtav1' never inserts keyframes at scene changes; set "
                                  "scene_change_detection to false");
            }
            if (mode == RateControl::Mode::Crf || mode == RateControl::Mode::ConstantQp)
            {
                const std::string parameter = mode == RateControl::Mode::Crf ? "crf" : "qp";
                setup.options[parameter] = number_text(scaled_value(rate_control, encoder_name, range, plan));
            }
            // Low-delay prediction: without it SVT-AV1 codes hierarchical
            // mini-GOPs, B-frames in all but name.
            setup.options["svtav1-params"] = "pred-struct=1:scd=0";
            plan.reports_qp = true;
        }
        else if (encoder_name == "librav1e")
        {
            require_mode(rate_control,
                         {RateControl::Mode::ConstantQp, RateControl::Mode::Bitrate, RateControl::Mode::Constrained},
                         encoder_name);
            refuse_b_frames(gop, encoder_name);
            if (mode == RateControl::Mode::ConstantQp)
            {
                setup.options["qp"] = number_text(scaled_value(rate_control, encoder_name, range, plan));
            }
            if (!gop.scene_change_detection)
            {
                setup.keyint_min = gop.keyframe_interval;
            }
        }
        else
        {
            throw NotImplemented("encode_video() with " + encoder_name);
        }

        plan.muxer = options.container.empty() ? default_muxer(options.codec) : options.container;
        const AVOutputFormat* muxer = av_guess_format(plan.muxer.c_str(), nullptr, nullptr);
        if (muxer == nullptr)
        {
            throw UnsupportedCapability("muxer", plan.muxer, build_info().identity_hash);
        }
        const std::string extensions = muxer->extensions != nullptr ? muxer->extensions : "";
        plan.extension = extensions.empty() ? plan.muxer : extensions.substr(0, extensions.find(','));

        add_caller_options(plan, options.encoder_options);
        return plan;
    }
}
