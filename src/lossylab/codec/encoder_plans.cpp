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

        /// The range and meaning of an encoder's quality parameter.
        struct QualityScale
        {
            double minimum = 0.0;
            double maximum = 0.0;
            bool integral = true;
            std::string description;
            bool higher_is_better = false;
        };

        /// The rate control's value, checked against the encoder's scale.
        double scaled_value(const RateControl& rate_control, const std::string& encoder_name,
                            const QualityScale& scale, EncoderPlan& plan)
        {
            const double value = rate_control.value();
            if ((scale.integral && value != std::floor(value)) || value < scale.minimum || value > scale.maximum)
            {
                throw ConfigError("encoder '" + encoder_name + "' takes " + scale.description + ", not " +
                                  number_text(value));
            }
            plan.quality_scale = scale.description;
            plan.quality_range = QualityRange{scale.minimum, scale.maximum, scale.integral, scale.higher_is_better};
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
            if (encoder_name == "libaom-av1")
            {
                const QualityScale scale{0, 63, true, "crf, an integer from 0 to 63, lower is better"};
                plan.setup.options["crf"] = number_text(scaled_value(rate_control, encoder_name, scale, plan));
                plan.setup.options["still-picture"] = "1";
            }
            else if (encoder_name == "libsvtav1")
            {
                const QualityScale scale{1, 63, true, "crf, an integer from 1 to 63, lower is better"};
                plan.setup.options["crf"] = number_text(scaled_value(rate_control, encoder_name, scale, plan));
                plan.reports_qp = true;
                plan.qp_scale = "SVT-AV1 QP, 0 to 63";
            }
            else if (encoder_name == "librav1e")
            {
                const QualityScale scale{0, 255, true, "a quantizer, an integer from 0 to 255, lower is better"};
                plan.setup.options["qp"] = number_text(scaled_value(rate_control, encoder_name, scale, plan));
            }
            else
            {
                throw NotImplemented("encode_image() with " + encoder_name);
            }
            plan.muxer = "avif";
            plan.extension = "avif";
        }
    }

    EncoderPlan plan_image_encode(const std::string& encoder_name, const EncodeImageOptions& options,
                                  const PixelFormat& pixel_format, const ColorSpec& color)
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

        case ImageCodec::Mjpeg:
        {
            if (options.lossless)
            {
                throw ConfigError("encode_image() has no lossless JPEG");
            }
            require_mode(rate_control, {RateControl::Mode::Quality}, encoder_name);
            const QualityScale scale{1, 31, true, "qscale, an integer from 1 to 31, lower is better"};
            const int qscale = static_cast<int>(scaled_value(rate_control, encoder_name, scale, plan));
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
            plan.setup.fixed_qscale = qscale;
            plan.setup.qmin = qscale;
            plan.setup.qmax = qscale;
            plan.block_grid = BlockGrid::for_kind(BlockGridKind::Dct8);
            plan.reports_qp = true;
            plan.qp_scale = "MJPEG qscale, 1 to 31";
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
                const QualityScale scale{0, 100, false, "an effort from 0 to 100, higher is slower and smaller"};
                plan.setup.options["quality"] = number_text(scaled_value(rate_control, encoder_name, scale, plan));
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
                const QualityScale scale{0, 100, false, "a quality from 0 to 100, higher is better", true};
                plan.setup.options["quality"] = number_text(scaled_value(rate_control, encoder_name, scale, plan));
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
                const QualityScale scale{0.01, 15, false,
                                         "a Butteraugli distance from 0.01 to 15, lower is better"};
                plan.setup.options["distance"] = number_text(scaled_value(rate_control, encoder_name, scale, plan));

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
            plan_avif(encoder_name, rate_control, plan);
            break;

        case ImageCodec::Heif:
            throw UnsupportedCapability("image muxer", "heif", build_info().build_id);
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

        // x264 and x265 extend the QP range by 6 per bit above 8.
        const double high_depth_extension = 6.0 * std::max(0, pixel_format.bit_depth() - 8);
        const RateControl::Mode mode = rate_control.mode();

        if (encoder_name == "libx264" || encoder_name == "libx265")
        {
            require_mode(rate_control,
                         {RateControl::Mode::Crf, RateControl::Mode::ConstantQp, RateControl::Mode::Bitrate,
                          RateControl::Mode::Constrained},
                         encoder_name);
            const double maximum = 51.0 + high_depth_extension;
            if (mode == RateControl::Mode::Crf)
            {
                const QualityScale scale{0, maximum, false,
                                         "crf from 0 to " + number_text(maximum) + ", lower is better"};
                setup.options["crf"] = number_text(scaled_value(rate_control, encoder_name, scale, plan));
            }
            else if (mode == RateControl::Mode::ConstantQp)
            {
                const QualityScale scale{0, maximum, true,
                                         "a QP, an integer from 0 to " + number_text(maximum) + ", lower is better"};
                setup.options["qp"] = number_text(scaled_value(rate_control, encoder_name, scale, plan));
            }
            setup.max_b_frames = gop.b_frames;
            plan.reports_qp = true;
            if (encoder_name == "libx264")
            {
                setup.options["b-pyramid"] = gop.b_pyramid ? "normal" : "none";
                setup.options["sc_threshold"] = gop.scene_change_detection ? "40" : "0";
                plan.block_grid = BlockGrid::for_kind(BlockGridKind::Macroblock16);
                plan.qp_scale = "H.264 QP";
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
                plan.qp_scale = "HEVC QP";
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
                const QualityScale scale{0, 63, true, "crf, an integer from 0 to 63, lower is better"};
                setup.options["crf"] = number_text(scaled_value(rate_control, encoder_name, scale, plan));
            }
            else if (mode == RateControl::Mode::ConstantQp)
            {
                const QualityScale scale{0, 63, true, "a quantizer, an integer from 0 to 63, lower is better"};
                const int quantizer = static_cast<int>(scaled_value(rate_control, encoder_name, scale, plan));
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
                plan.qp_scale = "VP9 quantizer, 0 to 63";
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
                const bool crf = mode == RateControl::Mode::Crf;
                const std::string parameter = crf ? "crf" : "qp";
                const std::string description =
                    (crf ? "crf" : "a QP") + std::string(", an integer from 1 to 63, lower is better");
                const QualityScale scale{1, 63, true, description};
                setup.options[parameter] = number_text(scaled_value(rate_control, encoder_name, scale, plan));
            }
            // Low-delay prediction: without it SVT-AV1 codes hierarchical
            // mini-GOPs, B-frames in all but name.
            setup.options["svtav1-params"] = "pred-struct=1:scd=0";
            plan.reports_qp = true;
            plan.qp_scale = "SVT-AV1 QP, 0 to 63";
        }
        else if (encoder_name == "librav1e")
        {
            require_mode(rate_control,
                         {RateControl::Mode::ConstantQp, RateControl::Mode::Bitrate, RateControl::Mode::Constrained},
                         encoder_name);
            refuse_b_frames(gop, encoder_name);
            if (mode == RateControl::Mode::ConstantQp)
            {
                const QualityScale scale{0, 255, true, "a quantizer, an integer from 0 to 255, lower is better"};
                setup.options["qp"] = number_text(scaled_value(rate_control, encoder_name, scale, plan));
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
            throw UnsupportedCapability("muxer", plan.muxer, build_info().build_id);
        }
        const std::string extensions = muxer->extensions != nullptr ? muxer->extensions : "";
        plan.extension = extensions.empty() ? plan.muxer : extensions.substr(0, extensions.find(','));

        add_caller_options(plan, options.encoder_options);
        return plan;
    }
}
