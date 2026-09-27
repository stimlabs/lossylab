#include "lossylab/codec/encode.hpp"

#include "encoder_plans.hpp"
#include "encoder_session.hpp"

#include "lossylab/convert/convert.hpp"
#include "lossylab/core/error.hpp"
#include "lossylab/core/json_io.hpp"
#include "lossylab/detail/ff_error.hpp"
#include "lossylab/env/capabilities.hpp"
#include "lossylab/io/decode_image.hpp"
#include "lossylab/io/video_reader.hpp"
#include "lossylab/measure/measure.hpp"

#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <set>

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

            // Every frame must share one format, ICC profile, orientation and
            // pixel shape: a mixed sequence would have to be converted
            // somewhere, and that conversion has to be a stage of its own
            // rather than a side effect of encoding.
            const FormatDescription first = frames.front().describe();
            for (std::size_t i = 1; i < frames.size(); ++i)
            {
                if (frames[i].describe() != first)
                {
                    throw ConfigError(std::string(what) + " received frame " + std::to_string(i) +
                                      " in a different format, ICC profile, orientation or pixel shape from frame "
                                      "0; convert first");
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

    namespace
    {
        void require_known_options(const CodecInfo& encoder, const std::map<std::string, std::string>& options)
        {
            for (const auto& [name, value] : options)
            {
                static_cast<void>(value);
                if (encoder.find_option(name) == nullptr)
                {
                    throw ConfigError("encoder '" + encoder.name + "' has no option '" + name + "'");
                }
            }
        }

        void require_accepted_format(const CodecInfo& encoder, const PixelFormat& pixel_format)
        {
            if (!encoder.accepts(pixel_format))
            {
                throw ConfigError("encoder '" + encoder.name + "' does not accept " + pixel_format.name() +
                                  "; convert explicitly before encoding");
            }
        }

        /// The frames in the pixel format and color the encoder is to receive.
        /// Frames already in them pass through; any other conversion is
        /// refused under Strict::Refuse and recorded otherwise.
        std::vector<Frame> fit_to_encoder(const std::vector<Frame>& frames, const PixelFormat& pixel_format,
                                          const std::optional<ColorSpec>& color, const Strict strict,
                                          ConversionList& conversions, const std::string& what)
        {
            const Frame& first = frames.front();
            const ColorSpec target_color = color.value_or(first.color());
            if (first.pixel_format() == pixel_format && first.color() == target_color)
            {
                return frames;
            }
            if (!color.has_value() && first.pixel_format().is_rgb() != pixel_format.is_rgb())
            {
                throw ConfigError(what + " converts " + first.pixel_format().name() + " to " + pixel_format.name() +
                                  ", which needs the color to encode in; set color");
            }
            if (strict == Strict::Refuse)
            {
                throw ConversionRefused(first.describe().pixel_format.name() + " " + first.color().describe(),
                                        pixel_format.name() + " " + target_color.describe(),
                                        what + ": the frames are not in the format and color to encode; convert "
                                               "them explicitly or allow a recorded conversion");
            }

            std::vector<Frame> converted;
            converted.reserve(frames.size());
            for (const Frame& frame : frames)
            {
                FrameResult result = convert(frame, pixel_format, target_color, Strict::AllowRecorded);
                if (converted.empty())
                {
                    for (ConversionEvent& event : result.record.conversions)
                    {
                        event.cause = ConversionCause::CodecConstraint;
                        conversions.push_back(std::move(event));
                    }
                }
                converted.push_back(std::move(result.frame));
            }
            return converted;
        }

        /// Hands the frame's ICC profile, orientation and pixel shape to the
        /// encoder.
        void set_embedded(const Frame& frame, detail::EncoderSetup& setup)
        {
            setup.icc_profile = frame.icc_profile();
            setup.orientation = frame.orientation();
            setup.sample_aspect_ratio = frame.sample_aspect_ratio();
        }

        std::string ratio_text(const Rational ratio)
        {
            return std::to_string(ratio.num) + ":" + std::to_string(ratio.den);
        }

        /// Reads back what the encoded output holds of `frame`'s ICC profile,
        /// orientation and pixel shape. Each one it lost is refused under
        /// Strict::Refuse and recorded otherwise, and `output` is set to what
        /// the output holds. A format probe() cannot read these from is
        /// decoded; what even decoding cannot tell counts as lost.
        void account_for_embedded(const Frame& frame, const std::vector<std::uint8_t>& bytes,
                                  const std::string& extension, const std::string& encoder_name, const Strict strict,
                                  StageRecord& record, const std::string& what)
        {
            const IccProfile* profile = frame.icc_profile();
            const std::optional<int> orientation = frame.orientation();
            const Rational sample_aspect_ratio = frame.sample_aspect_ratio();
            if (profile == nullptr && !orientation.has_value() && sample_aspect_ratio == Rational{1, 1})
            {
                return;
            }

            const Source written_source = Source::from_memory(bytes, extension);
            ProbeResult written = probe(written_source);
            const StreamInfo* stream = written.primary_video_stream();
            if (stream != nullptr && (stream->orientation_availability == Availability::NotSupportedByBuild ||
                                      stream->icc_profile_availability == Availability::NotSupportedByBuild))
            {
                written = decode_image(written_source).probe;
                stream = written.primary_video_stream();
            }
            const TileGrid* grid = written.primary_tile_grid();
            if (stream == nullptr && grid != nullptr && !grid->tiles.empty())
            {
                for (const StreamInfo& candidate : written.streams)
                {
                    if (candidate.index == grid->tiles.front().stream_index)
                    {
                        stream = &candidate;
                    }
                }
            }
            if (stream == nullptr)
            {
                throw Error(what + ": the encoded output holds no picture to check");
            }

            const std::optional<IccProfileInfo>& written_profile = grid != nullptr ? grid->icc_profile
                                                                                   : stream->icc_profile;
            std::optional<int> written_orientation = grid != nullptr ? grid->orientation : stream->orientation;
            if (written_orientation == 1)
            {
                written_orientation.reset();
            }

            const std::string context = what + ", whose output cannot hold it; clear it on the frame to drop it";
            if (profile != nullptr && !written_profile.has_value())
            {
                record_or_refuse(strict, record.conversions, "icc_profile", profile->info.name(), "dropped",
                                 ConversionCause::CodecConstraint, encoder_name, context);
                record.output.icc_profile.clear();
            }
            if (orientation != written_orientation)
            {
                record_or_refuse(strict, record.conversions, "orientation",
                                 orientation.has_value() ? std::to_string(*orientation) : "none",
                                 written_orientation.has_value() ? std::to_string(*written_orientation) : "dropped",
                                 ConversionCause::CodecConstraint, encoder_name, context);
                record.output.orientation = written_orientation;
            }
            if (stream->sample_aspect_ratio != sample_aspect_ratio)
            {
                record_or_refuse(strict, record.conversions, "sample_aspect_ratio", ratio_text(sample_aspect_ratio),
                                 ratio_text(stream->sample_aspect_ratio), ConversionCause::CodecConstraint,
                                 encoder_name, context);
                record.output.sample_aspect_ratio = stream->sample_aspect_ratio;
            }
        }

        /// One FrameStats per input frame, from the packets that coded it,
        /// matched by timestamp.
        std::vector<FrameStats> frame_stats(const std::vector<detail::PacketPtr>& packets, const bool reports_qp)
        {
            std::map<std::int64_t, FrameStats> by_pts;
            for (const detail::PacketPtr& packet : packets)
            {
                const FrameStats stats = detail::packet_stats(*packet, reports_qp);
                const auto [it, inserted] = by_pts.try_emplace(stats.pts, stats);
                if (!inserted)
                {
                    it->second.size_bytes = it->second.size_bytes.value_or(0) + stats.size_bytes.value_or(0);
                }
            }
            std::vector<FrameStats> frames;
            for (auto& [pts, stats] : by_pts)
            {
                stats.index = static_cast<int>(pts);
                frames.push_back(stats);
            }
            return frames;
        }

        double bits_per_pixel(const std::size_t bytes, const FormatDescription& format, const std::size_t frames)
        {
            return static_cast<double>(bytes) * 8.0 /
                   (static_cast<double>(format.width) * format.height * static_cast<double>(frames));
        }

        /// One record for an encode followed by a decode of its output.
        StageRecord roundtrip_record(const StageRecord& encoded, const StageRecord& decoded, StageEvidence evidence)
        {
            StageRecord record;
            record.evidence = std::move(evidence);
            record.implementation = encoded.implementation + "+" + decoded.implementation;
            record.input = encoded.input;
            record.output = decoded.output;
            record.conversions = encoded.conversions;
            record.conversions.insert(record.conversions.end(), decoded.conversions.begin(),
                                      decoded.conversions.end());
            record.transform = CoordinateTransform::identity();
            record.block_grid = encoded.block_grid;
            record.frames = encoded.frames;
            record.reproducible = encoded.reproducible && decoded.reproducible;
            record.duration_ms = encoded.duration_ms + decoded.duration_ms;
            record.ffmpeg_duration_ms = encoded.ffmpeg_duration_ms + decoded.ffmpeg_duration_ms;
            return record;
        }

        /// Decodes an encoded image, assuming the encoded color for whatever
        /// the file leaves untagged.
        FrameResult decode_encoded(const EncodedResult& encoded, const DecodeSpec& decode_spec)
        {
            DecodeImageOptions options;
            options.pixel_format = decode_spec.pixel_format;
            options.color = decode_spec.color;
            options.assumed_color = encoded.record.output.color;
            options.strict = decode_spec.strict;
            const std::string& extension = std::get<EncodeImageEvidence>(encoded.record.evidence).extension;
            DecodedImage decoded = decode_image(Source::from_memory(encoded.bytes, extension), options);
            return FrameResult{std::move(decoded.frame), std::move(decoded.record), decoded.configuration};
        }

        /// Decodes an encoded clip, which must give back `frame_count` frames.
        FramesResult decode_encoded(const EncodedResult& encoded, const DecodeSpec& decode_spec,
                                    const std::size_t frame_count)
        {
            if (decode_spec.thread_count < 1)
            {
                throw ConfigError("roundtrip() decode thread_count must be at least 1");
            }
            VideoReaderOptions options;
            options.pixel_format = decode_spec.pixel_format;
            options.color = decode_spec.color;
            options.assumed_color = encoded.record.output.color;
            options.thread_count = decode_spec.thread_count;
            options.strict = decode_spec.strict;
            const std::string& extension = std::get<EncodeVideoEvidence>(encoded.record.evidence).extension;

            VideoReader reader(Source::from_memory(encoded.bytes, extension), options);
            std::vector<VideoFrame> decoded = reader.frames(FrameSelector::all());
            if (decoded.size() != frame_count)
            {
                throw Error("roundtrip(): " + std::to_string(frame_count) + " frames encoded, but " +
                            std::to_string(decoded.size()) + " decoded");
            }

            FramesResult result;
            result.frames.reserve(decoded.size());
            for (VideoFrame& frame : decoded)
            {
                result.frames.push_back(std::move(frame.frame));
            }
            result.record = reader.record();
            result.configuration = reader.configuration();
            return result;
        }
    }

    EncodedResult encode_video(const std::vector<Frame>& frames,
                               const EncodeVideoOptions& options)
    {
        const detail::StageClock clock;
        validate_common(frames, options.pixel_format, options.thread_count, "encode_video()");

        // Resolved before anything else, so a codec this build cannot provide
        // is reported by name rather than as a failure deep inside FFmpeg.
        const CodecInfo& encoder =
            capabilities().require_encoder(options.codec, options.backend);
        if (options.backend != EncoderBackend::Software)
        {
            throw NotImplemented("encode_video() with a hardware backend");
        }
        require_accepted_format(encoder, options.pixel_format);
        require_known_options(encoder, options.encoder_options);

        StageRecord record;
        record.implementation = encoder.name;
        record.input = frames.front().describe();
        record.transform = CoordinateTransform::identity();

        const std::vector<Frame> encoded_frames = fit_to_encoder(
            frames, options.pixel_format, options.color, options.strict, record.conversions, "encode_video()");
        const Frame& first = encoded_frames.front();
        detail::EncoderPlan plan = detail::plan_video_encode(encoder.name, options, first.pixel_format());
        plan.setup.width = first.width();
        plan.setup.height = first.height();
        plan.setup.pixel_format = first.pixel_format();
        plan.setup.color = first.color();
        plan.setup.frame_rate = options.frame_rate;
        plan.setup.thread_count = options.thread_count;
        plan.setup.global_header = detail::muxer_wants_global_header(plan.muxer);
        set_embedded(first, plan.setup);

        detail::EncoderSession session(plan.setup);
        for (std::size_t index = 0; index < encoded_frames.size(); ++index)
        {
            session.send(encoded_frames[index], static_cast<std::int64_t>(index));
        }
        session.finish();

        record.frames = frame_stats(session.packets(), plan.reports_qp);
        std::vector<std::uint8_t> bytes = detail::mux_packets(plan.muxer, session.context(), session.packets());

        record.output = first.describe();
        account_for_embedded(first, bytes, plan.extension, encoder.name, options.strict, record, "encode_video()");
        record.block_grid = plan.block_grid;
        record.evidence = EncodeVideoEvidence{plan.muxer,
                                              plan.extension,
                                              static_cast<int>(encoded_frames.size()),
                                              first.color(),
                                              session.resolved_settings(),
                                              bits_per_pixel(bytes.size(), record.output, encoded_frames.size()),
                                              std::nullopt};
        record.duration_ms = clock.duration_ms();
        record.ffmpeg_duration_ms = clock.ffmpeg_duration_ms();
        return EncodedResult{std::move(bytes), std::move(record), options};
    }

    EncodedResult encode_image(const Frame& frame, const EncodeImageOptions& options)
    {
        const detail::StageClock clock;
        validate_common({frame}, options.pixel_format, options.thread_count, "encode_image()");

        const CodecInfo& encoder = capabilities().require_encoder(options.codec);
        require_accepted_format(encoder, options.pixel_format);
        require_known_options(encoder, options.encoder_options);

        StageRecord record;
        record.implementation = encoder.name;
        record.input = frame.describe();
        record.transform = CoordinateTransform::identity();

        const Frame encoded_frame = fit_to_encoder({frame}, options.pixel_format, options.color, options.strict,
                                                   record.conversions, "encode_image()")
                                        .front();
        detail::EncoderPlan plan = detail::plan_image_encode(encoder.name, options, encoded_frame.pixel_format(),
                                                             encoded_frame.color());
        plan.setup.width = encoded_frame.width();
        plan.setup.height = encoded_frame.height();
        plan.setup.pixel_format = encoded_frame.pixel_format();
        plan.setup.color = encoded_frame.color();
        plan.setup.thread_count = options.thread_count;
        plan.setup.global_header = !plan.muxer.empty() && detail::muxer_wants_global_header(plan.muxer);
        set_embedded(encoded_frame, plan.setup);

        detail::EncoderSession session(plan.setup);
        session.send(encoded_frame, 0);
        session.finish();
        if (session.packets().size() != 1)
        {
            throw Error("encode_image(): encoder '" + encoder.name + "' produced " +
                        std::to_string(session.packets().size()) + " packets for one image");
        }

        record.frames = frame_stats(session.packets(), plan.reports_qp);
        std::vector<std::uint8_t> bytes = plan.muxer.empty()
                                              ? detail::concatenate_packets(session.packets())
                                              : detail::mux_packets(plan.muxer, session.context(), session.packets());

        record.output = encoded_frame.describe();
        account_for_embedded(encoded_frame, bytes, plan.extension, encoder.name, options.strict, record,
                             "encode_image()");
        record.block_grid = plan.block_grid;
        record.evidence = EncodeImageEvidence{
            plan.muxer.empty() ? std::nullopt : std::optional<std::string>(plan.muxer),
            plan.extension,
            encoded_frame.color(),
            session.resolved_settings(),
            bits_per_pixel(bytes.size(), record.output, 1),
            std::nullopt};
        record.duration_ms = clock.duration_ms();
        record.ffmpeg_duration_ms = clock.ffmpeg_duration_ms();
        return EncodedResult{std::move(bytes), std::move(record), options};
    }

    FramesResult roundtrip(const std::vector<Frame>& frames,
                           const EncodeVideoOptions& encode_spec,
                           const DecodeSpec& decode_spec)
    {
        validate_common(frames, encode_spec.pixel_format, encode_spec.thread_count,
                        "roundtrip()");
        static_cast<void>(capabilities().require_encoder(encode_spec.codec, encode_spec.backend));
        static_cast<void>(capabilities().require_decoder(encode_spec.codec));

        const EncodedResult encoded = encode_video(frames, encode_spec);
        FramesResult decoded = decode_encoded(encoded, decode_spec, frames.size());
        RoundtripVideoEvidence evidence{std::get<EncodeVideoEvidence>(encoded.record.evidence),
                                        std::get<DecodeVideoEvidence>(decoded.record.evidence)};
        decoded.record = roundtrip_record(encoded.record, decoded.record, std::move(evidence));
        decoded.configuration =
            RoundtripVideoConfiguration{std::get<EncodeVideoOptions>(encoded.configuration),
                                        std::get<DecodeVideoConfiguration>(decoded.configuration)};
        return decoded;
    }

    FrameResult roundtrip(const Frame& frame, const EncodeImageOptions& encode_spec,
                          const DecodeSpec& decode_spec)
    {
        validate_common({frame}, encode_spec.pixel_format, encode_spec.thread_count,
                        "roundtrip()");
        static_cast<void>(capabilities().require_encoder(encode_spec.codec));
        static_cast<void>(capabilities().require_decoder(encode_spec.codec));

        const EncodedResult encoded = encode_image(frame, encode_spec);
        FrameResult decoded = decode_encoded(encoded, decode_spec);
        RoundtripImageEvidence evidence{std::get<EncodeImageEvidence>(encoded.record.evidence),
                                        std::get<DecodeImageEvidence>(decoded.record.evidence)};
        decoded.record = roundtrip_record(encoded.record, decoded.record, std::move(evidence));
        decoded.configuration = RoundtripImageConfiguration{std::get<EncodeImageOptions>(encoded.configuration),
                                                            std::get<DecodeImageOptions>(decoded.configuration)};
        return decoded;
    }

    namespace
    {
        /// The frames' own format and color, for decoding an encode of them
        /// back into something comparable with them.
        DecodeSpec decode_spec_like(const Frame& frame)
        {
            DecodeSpec decode_spec;
            decode_spec.pixel_format = frame.pixel_format();
            decode_spec.color = frame.color();
            decode_spec.strict = Strict::AllowRecorded;
            return decode_spec;
        }

        double pooled_metric(const std::vector<Frame>& reference, const std::vector<Frame>& distorted,
                             const EncodeTarget::Kind kind)
        {
            const bool psnr = kind == EncodeTarget::Kind::Psnr;
            CompareOptions options;
            options.metrics = {psnr ? Metric::Psnr : Metric::Ssim};
            options.strict = Strict::AllowRecorded;
            return compare(reference, distorted, options).evidence().pooled.at(psnr ? "psnr_mean" : "ssim_mean");
        }

        /// Bisects the encoder's quality range, ordered from its worst
        /// quality to its best so that bits per pixel, PSNR and SSIM all grow
        /// along it. Keeps the attempt closest to the target.
        EncodeToTargetResult search_quality(const EncodeTarget& target, const detail::QualityRange& range,
                                            const std::function<EncodedResult(double)>& encode,
                                            const std::function<double(const EncodedResult&)>& achieved_by)
        {
            const double worst = range.higher_is_better ? range.minimum : range.maximum;
            const double best = range.higher_is_better ? range.maximum : range.minimum;
            const auto parameter_at = [&](const double position)
            {
                const double parameter = worst + position * (best - worst);
                return range.integral ? std::round(parameter) : parameter;
            };

            EncodeToTargetResult result;
            EncodeSearch search;
            search.target = target;
            double closest_distance = std::numeric_limits<double>::infinity();
            std::set<double> tried;
            double low = 0.0;
            double high = 1.0;
            for (int iteration = 0; iteration < target.max_iterations; ++iteration)
            {
                const double position = (low + high) / 2.0;
                const double parameter = parameter_at(position);
                if (!tried.insert(parameter).second)
                {
                    // An integer scale has run out of values between the bounds.
                    break;
                }

                EncodedResult encoded = encode(parameter);
                const double achieved = achieved_by(encoded);
                search.attempts.push_back(EncodeAttempt{parameter, achieved});

                const double distance = std::abs(achieved - target.value);
                if (distance < closest_distance || result.bytes.empty())
                {
                    closest_distance = distance;
                    result.bytes = std::move(encoded.bytes);
                    result.record = std::move(encoded.record);
                    result.configuration = std::move(encoded.configuration);
                    search.quality_parameter = parameter;
                    search.achieved = achieved;
                }
                if (distance <= target.tolerance)
                {
                    search.converged = true;
                    break;
                }
                (achieved < target.value ? low : high) = position;
            }

            std::visit(
                [&search](auto& evidence)
                {
                    if constexpr (requires { evidence.search = search; })
                    {
                        evidence.search = search;
                    }
                },
                result.record.evidence);
            return result;
        }

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

    const EncodeSearch& EncodeToTargetResult::search() const
    {
        if (const auto* image = std::get_if<EncodeImageEvidence>(&record.evidence);
            image != nullptr && image->search.has_value())
        {
            return *image->search;
        }
        if (const auto* video = std::get_if<EncodeVideoEvidence>(&record.evidence);
            video != nullptr && video->search.has_value())
        {
            return *video->search;
        }
        throw Error("encode_to_target() result has no search in its record's evidence");
    }

    EncodeToTargetResult encode_to_target(const std::vector<Frame>& frames,
                                          const EncodeVideoOptions& options,
                                          const EncodeTarget& target)
    {
        validate_common(frames, options.pixel_format, options.thread_count,
                        "encode_to_target()");
        const CodecInfo& encoder = capabilities().require_encoder(options.codec, options.backend);
        validate_target(target, options.rate_control);
        if (target.kind == EncodeTarget::Kind::Vmaf)
        {
            throw NotImplemented("encode_to_target() for vmaf");
        }

        const std::optional<detail::QualityRange> range =
            detail::plan_video_encode(encoder.name, options, options.pixel_format).quality_range;
        if (!range.has_value())
        {
            throw ConfigError("encode_to_target(): '" + options.rate_control.describe() +
                              "' has no quality range for " + encoder.name + " to search");
        }

        const DecodeSpec decode_spec = decode_spec_like(frames.front());
        return search_quality(
            target, *range,
            [&](const double parameter)
            {
                EncodeVideoOptions attempt = options;
                attempt.rate_control = options.rate_control.with_quality_parameter(parameter);
                return encode_video(frames, attempt);
            },
            [&](const EncodedResult& encoded)
            {
                if (target.kind == EncodeTarget::Kind::BitsPerPixel)
                {
                    return encoded.bits_per_pixel();
                }
                return pooled_metric(frames, decode_encoded(encoded, decode_spec, frames.size()).frames, target.kind);
            });
    }

    EncodeToTargetResult encode_to_target(const Frame& frame,
                                          const EncodeImageOptions& options,
                                          const EncodeTarget& target)
    {
        validate_common({frame}, options.pixel_format, options.thread_count,
                        "encode_to_target()");
        const CodecInfo& encoder = capabilities().require_encoder(options.codec);
        validate_target(target, options.rate_control);
        if (target.kind == EncodeTarget::Kind::Vmaf)
        {
            throw NotImplemented("encode_to_target() for vmaf");
        }
        if (options.lossless)
        {
            throw ConfigError("encode_to_target(): a lossless encode has no quality to search");
        }

        const ColorSpec color = options.color.value_or(frame.color());
        const std::optional<detail::QualityRange> range =
            detail::plan_image_encode(encoder.name, options, options.pixel_format, color).quality_range;
        if (!range.has_value())
        {
            throw ConfigError("encode_to_target(): " + encoder.name + " has no quality parameter to search");
        }

        const DecodeSpec decode_spec = decode_spec_like(frame);
        return search_quality(
            target, *range,
            [&](const double parameter)
            {
                EncodeImageOptions attempt = options;
                attempt.rate_control = options.rate_control.with_quality_parameter(parameter);
                return encode_image(frame, attempt);
            },
            [&](const EncodedResult& encoded)
            {
                if (target.kind == EncodeTarget::Kind::BitsPerPixel)
                {
                    return encoded.bits_per_pixel();
                }
                return pooled_metric({frame}, {decode_encoded(encoded, decode_spec).frame}, target.kind);
            });
    }
}
