#include "lossylab/codec/encode.hpp"

#include "lossylab/core/error.hpp"
#include "lossylab/env/capabilities.hpp"

namespace lossylab
{
    namespace
    {
        std::string mode_name(const RateControl::Mode mode)
        {
            switch (mode)
            {
            case RateControl::Mode::Crf: return "crf";
            case RateControl::Mode::ConstantQp: return "qp";
            case RateControl::Mode::Bitrate: return "bitrate";
            case RateControl::Mode::Constrained: return "constrained";
            case RateControl::Mode::Quality: return "quality";
            }
            return "unknown";
        }

        RateControl::Mode mode_from_name(const std::string& name)
        {
            if (name == "crf") { return RateControl::Mode::Crf; }
            if (name == "qp") { return RateControl::Mode::ConstantQp; }
            if (name == "bitrate") { return RateControl::Mode::Bitrate; }
            if (name == "constrained") { return RateControl::Mode::Constrained; }
            if (name == "quality") { return RateControl::Mode::Quality; }
            throw ConfigError("unknown rate control mode '" + name + "'");
        }

        /// Validates what can be checked before any encoding happens, so a bad
        /// spec fails immediately rather than partway through a dataset.
        void validate_common(const std::vector<Frame>& frames, const PixelFormat& pixel_format,
                             const int thread_count, const char* what)
        {
            if (frames.empty())
            {
                throw ConfigError(std::string(what) + " received no frames");
            }
            for (const Frame& frame : frames)
            {
                if (frame.empty())
                {
                    throw ConfigError(std::string(what) + " received an empty frame");
                }
                frame.color().require_fully_specified(what);
            }
            if (!pixel_format.is_valid())
            {
                throw ConfigError(std::string(what) + " requires an explicit pixel format");
            }
            if (thread_count < 1)
            {
                throw ConfigError(std::string(what) + " thread_count must be at least 1");
            }

            // Every frame must share one format: a mixed sequence would have to
            // be converted somewhere, and that conversion has to be a stage of
            // its own rather than a side effect of encoding.
            const FormatDescription first = frames.front().describe();
            for (std::size_t i = 1; i < frames.size(); ++i)
            {
                if (frames[i].describe() != first)
                {
                    throw ConfigError(std::string(what) + " received frame " +
                                      std::to_string(i) +
                                      " in a different format from frame 0; convert first");
                }
            }
        }
    }

    // -----------------------------------------------------------------------
    // RateControl
    // -----------------------------------------------------------------------

    RateControl RateControl::crf(const double value)
    {
        RateControl control;
        control.m_mode = Mode::Crf;
        control.m_value = value;
        return control;
    }

    RateControl RateControl::constant_qp(const int qp)
    {
        RateControl control;
        control.m_mode = Mode::ConstantQp;
        control.m_value = qp;
        return control;
    }

    RateControl RateControl::bitrate(const std::int64_t bits_per_second)
    {
        if (bits_per_second <= 0)
        {
            throw ConfigError("bitrate must be positive");
        }
        RateControl control;
        control.m_mode = Mode::Bitrate;
        control.m_rate = bits_per_second;
        return control;
    }

    RateControl RateControl::constrained(const std::int64_t bits_per_second,
                                         const std::int64_t max_rate,
                                         const std::int64_t buffer_size)
    {
        if (bits_per_second <= 0 || max_rate <= 0 || buffer_size <= 0)
        {
            throw ConfigError("constrained rate control needs positive rate, max rate and "
                              "buffer size");
        }
        if (max_rate < bits_per_second)
        {
            throw ConfigError("constrained rate control max rate is below the average rate");
        }
        RateControl control;
        control.m_mode = Mode::Constrained;
        control.m_rate = bits_per_second;
        control.m_max_rate = max_rate;
        control.m_buffer_size = buffer_size;
        return control;
    }

    RateControl RateControl::quality(const double value)
    {
        RateControl control;
        control.m_mode = Mode::Quality;
        control.m_value = value;
        return control;
    }

    std::optional<double> RateControl::quality_parameter() const noexcept
    {
        switch (m_mode)
        {
        case Mode::Crf:
        case Mode::ConstantQp:
        case Mode::Quality:
            return m_value;
        case Mode::Bitrate:
        case Mode::Constrained:
            // Quality is an outcome here, not an input, so there is no
            // parameter for a search to move.
            return std::nullopt;
        }
        return std::nullopt;
    }

    RateControl RateControl::with_quality_parameter(const double value) const
    {
        if (!quality_parameter().has_value())
        {
            throw ConfigError("rate control mode '" + mode_name(m_mode) +
                              "' has no quality parameter to set");
        }
        RateControl control = *this;
        control.m_value = value;
        return control;
    }

    std::string RateControl::describe() const
    {
        switch (m_mode)
        {
        case Mode::Crf: return "crf " + std::to_string(m_value);
        case Mode::ConstantQp: return "qp " + std::to_string(static_cast<int>(m_value));
        case Mode::Quality: return "quality " + std::to_string(m_value);
        case Mode::Bitrate: return "bitrate " + std::to_string(m_rate);
        case Mode::Constrained:
            return "constrained " + std::to_string(m_rate) + "/" + std::to_string(m_max_rate) +
                   " vbv " + std::to_string(m_buffer_size);
        }
        return "unknown";
    }

    json::Value RateControl::to_json() const
    {
        return json::object({
            {"mode", mode_name(m_mode)},
            {"value", m_value},
            {"rate", m_rate},
            {"max_rate", m_max_rate},
            {"buffer_size", m_buffer_size},
        });
    }

    RateControl RateControl::from_json(const json::Value& value)
    {
        RateControl control;
        control.m_mode = mode_from_name(value.at("mode").get<std::string>());
        control.m_value = json::double_or(value, "value", 0.0);
        control.m_rate = json::int_or(value, "rate", 0);
        control.m_max_rate = json::int_or(value, "max_rate", 0);
        control.m_buffer_size = json::int_or(value, "buffer_size", 0);
        return control;
    }

    // -----------------------------------------------------------------------
    // GopStructure
    // -----------------------------------------------------------------------

    GopStructure GopStructure::intra_only() noexcept
    {
        GopStructure gop;
        gop.keyframe_interval = 1;
        gop.b_frames = 0;
        gop.scene_change_detection = false;
        gop.closed_gop = true;
        return gop;
    }

    json::Value GopStructure::to_json() const
    {
        return json::object({
            {"keyframe_interval", keyframe_interval},
            {"b_frames", b_frames},
            {"scene_change_detection", scene_change_detection},
            {"b_pyramid", b_pyramid},
            {"closed_gop", closed_gop},
        });
    }

    GopStructure GopStructure::from_json(const json::Value& value)
    {
        GopStructure gop;
        gop.keyframe_interval =
            static_cast<int>(value.at("keyframe_interval").get<std::int64_t>());
        gop.b_frames = static_cast<int>(value.at("b_frames").get<std::int64_t>());
        gop.scene_change_detection = json::bool_or(value, "scene_change_detection", true);
        gop.b_pyramid = json::bool_or(value, "b_pyramid", false);
        gop.closed_gop = json::bool_or(value, "closed_gop", false);
        return gop;
    }

    // -----------------------------------------------------------------------
    // EncodeTarget
    // -----------------------------------------------------------------------

    std::string EncodeTarget::describe() const
    {
        switch (kind)
        {
        case Kind::BitsPerPixel: return "bpp " + std::to_string(value);
        case Kind::Psnr: return "psnr " + std::to_string(value);
        case Kind::Ssim: return "ssim " + std::to_string(value);
        case Kind::Vmaf: return "vmaf " + std::to_string(value);
        }
        return "unknown";
    }

    json::Value EncodeTarget::to_json() const
    {
        const char* kind_name = "bpp";
        switch (kind)
        {
        case Kind::BitsPerPixel: kind_name = "bpp"; break;
        case Kind::Psnr: kind_name = "psnr"; break;
        case Kind::Ssim: kind_name = "ssim"; break;
        case Kind::Vmaf: kind_name = "vmaf"; break;
        }
        return json::object({
            {"kind", kind_name},
            {"value", value},
            {"tolerance", tolerance},
            {"max_iterations", max_iterations},
        });
    }

    EncodeTarget EncodeTarget::from_json(const json::Value& value)
    {
        EncodeTarget target;
        const std::string& kind_name = value.at("kind").get_ref<const std::string&>();
        if (kind_name == "bpp") { target.kind = Kind::BitsPerPixel; }
        else if (kind_name == "psnr") { target.kind = Kind::Psnr; }
        else if (kind_name == "ssim") { target.kind = Kind::Ssim; }
        else if (kind_name == "vmaf") { target.kind = Kind::Vmaf; }
        else { throw ConfigError("unknown encode target '" + kind_name + "'"); }

        target.value = value.at("value").get<double>();
        target.tolerance = json::double_or(value, "tolerance", 0.02);
        target.max_iterations = static_cast<int>(json::int_or(value, "max_iterations", 8));
        return target;
    }

    // -----------------------------------------------------------------------
    // Encoding
    // -----------------------------------------------------------------------

    EncodedResult encode_video(const std::vector<Frame>& frames,
                               const EncodeVideoOptions& options)
    {
        validate_common(frames, options.pixel_format, options.thread_count, "encode_video()");

        // Resolved before anything else, so a codec this build cannot provide
        // is reported by name rather than as a failure deep inside FFmpeg.
        const CodecInfo& encoder =
            capabilities().require_encoder(options.codec, options.backend);

        if (!encoder.accepts(options.pixel_format))
        {
            throw ConfigError("encoder '" + encoder.name + "' does not accept " +
                              options.pixel_format.name() +
                              "; convert explicitly before encoding");
        }

        for (const auto& [name, value] : options.encoder_options)
        {
            static_cast<void>(value);
            if (encoder.find_option(name) == nullptr)
            {
                throw ConfigError("encoder '" + encoder.name + "' has no option '" + name + "'");
            }
        }

        LL_NOT_IMPLEMENTED();
    }

    EncodedResult encode_image(const Frame& frame, const EncodeImageOptions& options)
    {
        validate_common({frame}, options.pixel_format, options.thread_count, "encode_image()");

        const CodecInfo& encoder = capabilities().require_encoder(options.codec);
        if (!encoder.accepts(options.pixel_format))
        {
            throw ConfigError("encoder '" + encoder.name + "' does not accept " +
                              options.pixel_format.name() +
                              "; convert explicitly before encoding");
        }

        LL_NOT_IMPLEMENTED();
    }

    FramesResult roundtrip(const std::vector<Frame>& frames,
                           const EncodeVideoOptions& encode_spec,
                           const DecodeSpec& decode_spec)
    {
        static_cast<void>(decode_spec);
        validate_common(frames, encode_spec.pixel_format, encode_spec.thread_count,
                        "roundtrip()");
        static_cast<void>(capabilities().require_encoder(encode_spec.codec, encode_spec.backend));
        static_cast<void>(capabilities().require_decoder(encode_spec.codec));

        LL_NOT_IMPLEMENTED();
    }

    FrameResult roundtrip(const Frame& frame, const EncodeImageOptions& encode_spec,
                          const DecodeSpec& decode_spec)
    {
        static_cast<void>(decode_spec);
        validate_common({frame}, encode_spec.pixel_format, encode_spec.thread_count,
                        "roundtrip()");
        static_cast<void>(capabilities().require_encoder(encode_spec.codec));
        static_cast<void>(capabilities().require_decoder(encode_spec.codec));

        LL_NOT_IMPLEMENTED();
    }

    namespace
    {
        void validate_target(const EncodeTarget& target, const RateControl& rate_control)
        {
            if (target.tolerance <= 0.0)
            {
                throw ConfigError("encode target tolerance must be positive");
            }
            if (target.max_iterations < 1)
            {
                throw ConfigError("encode target needs at least one iteration");
            }
            if (!rate_control.quality_parameter().has_value())
            {
                throw ConfigError("encode_to_target() needs a rate control mode with a quality "
                                  "parameter to search; '" +
                                  rate_control.describe() + "' has none");
            }

            switch (target.kind)
            {
            case EncodeTarget::Kind::Psnr:
                capabilities().require_metric(Metric::Psnr);
                break;
            case EncodeTarget::Kind::Ssim:
                capabilities().require_metric(Metric::Ssim);
                break;
            case EncodeTarget::Kind::Vmaf:
                capabilities().require_metric(Metric::Vmaf);
                break;
            case EncodeTarget::Kind::BitsPerPixel:
                // Measured from the coded size; no filter needed.
                break;
            }
        }
    }

    EncodeToTargetResult encode_to_target(const std::vector<Frame>& frames,
                                          const EncodeVideoOptions& options,
                                          const EncodeTarget& target)
    {
        validate_common(frames, options.pixel_format, options.thread_count,
                        "encode_to_target()");
        static_cast<void>(capabilities().require_encoder(options.codec, options.backend));
        validate_target(target, options.rate_control);

        LL_NOT_IMPLEMENTED();
    }

    EncodeToTargetResult encode_to_target(const Frame& frame,
                                          const EncodeImageOptions& options,
                                          const EncodeTarget& target)
    {
        validate_common({frame}, options.pixel_format, options.thread_count,
                        "encode_to_target()");
        static_cast<void>(capabilities().require_encoder(options.codec));
        validate_target(target, options.rate_control);

        LL_NOT_IMPLEMENTED();
    }
}
