#include "lossylab/io/read_headers.hpp"

#include "lossylab/core/color_spec.hpp"
#include "lossylab/core/error.hpp"
#include "lossylab/core/json_io.hpp"
#include "lossylab/core/schema_version.hpp"
#include "lossylab/detail/ff_error.hpp"
#include "lossylab/detail/ff_ptr.hpp"
#include "lossylab/detail/log_capture.hpp"
#include "lossylab/env/build_info.hpp"
#include "lossylab/io/input_context.hpp"

#include <algorithm>
#include <cinttypes>
#include <cstring>
#include <exception>
#include <map>
#include <string_view>
#include <utility>

namespace lossylab
{
    json::Value ParameterSet::to_json() const
    {
        return json::object({{"kind", kind}, {"id", id}, {"fields", fields}});
    }

    json::Value SliceInfo::to_json() const
    {
        return json::object({
            {"index", index},
            {"slice_type", slice_type},
            {"qp", json::optional_or_null(qp)},
            {"size_bytes", json::optional_or_null(size_bytes)},
        });
    }

    json::Value HeaderInfo::to_json() const
    {
        return json::object({
            {"schema_version", schema_version},
            {"codec_name", codec_name},
            {"embedded_encoder_settings", json::optional_or_null(embedded_encoder_settings)},
            {"embedded_encoder_settings_availability",
             to_string(embedded_encoder_settings_availability)},
            {"encoder_settings", json::to_object(encoder_settings)},
            {"parameter_sets", json::to_array(parameter_sets)},
            {"slices", json::to_array(slices)},
            {"quantizer_indices", json::to_array(quantizer_indices)},
            {"bitstream_color", bitstream_color},
        });
    }

    namespace
    {
        // The format strings FFmpeg's coded-bitstream layer (libavcodec/cbs.c)
        // and the trace_headers filter log with. A call is read only when its
        // format matches one of these exactly, so a future FFmpeg that changes
        // them yields no units rather than misread arguments.
        constexpr const char* element_format = "%-10d  %s%*s = %" PRId64 "\n";
        constexpr const char* unit_format = "%s\n";
        constexpr const char* packet_format = "Packet: %d bytes%s.\n";

        /// Warned for every High-profile avcC: its trailing chroma format,
        /// bit depth and SPS-extension fields, which the parser does not read.
        constexpr const char* avcc_remainder_format = "%u bytes left at end of AVCC header.\n";

        /// One syntax structure the trace named, with its elements in
        /// bitstream order.
        struct TraceUnit
        {
            std::string name;
            std::vector<std::pair<std::string, std::int64_t>> elements;

            [[nodiscard]] std::optional<std::int64_t> value(const std::string_view element) const
            {
                for (const auto& [name_in_unit, value] : elements)
                {
                    if (name_in_unit == element)
                    {
                        return value;
                    }
                }
                return std::nullopt;
            }

            [[nodiscard]] std::int64_t value_or(const std::string_view element, const std::int64_t fallback) const
            {
                return value(element).value_or(fallback);
            }

            /// Every element as a JSON object. A name that repeats, such as
            /// an alignment bit, keeps its last value.
            [[nodiscard]] json::Value fields() const
            {
                json::Value result = json::Value::object();
                for (const auto& [name_in_unit, value] : elements)
                {
                    result[name_in_unit] = value;
                }
                return result;
            }
        };

        /// Gathers trace_headers' log calls into units. Runs inside FFmpeg's
        /// log callback, so it never throws; a failure is kept and rethrown by
        /// `rethrow_failure()` once control is back in the library.
        class TraceCollector
        {
        public:
            bool on_log(const int level, const char* format, va_list args) noexcept
            {
                try
                {
                    if (std::strcmp(format, element_format) == 0)
                    {
                        static_cast<void>(va_arg(args, int));  // bit position
                        const char* name = va_arg(args, const char*);
                        static_cast<void>(va_arg(args, int));          // padding width
                        static_cast<void>(va_arg(args, const char*));  // the bits themselves
                        const std::int64_t value = va_arg(args, std::int64_t);
                        if (m_units.empty())
                        {
                            m_units.emplace_back();
                        }
                        m_units.back().elements.emplace_back(name, value);
                        ++m_element_count;
                        return true;
                    }
                    if (std::strcmp(format, unit_format) == 0)
                    {
                        m_units.push_back(TraceUnit{va_arg(args, const char*), {}});
                        return true;
                    }
                    if (std::strcmp(format, packet_format) == 0 || std::strcmp(format, avcc_remainder_format) == 0)
                    {
                        return true;
                    }
                }
                catch (...)
                {
                    m_failure = std::current_exception();
                    return true;
                }

                // Remaining informational lines ("Extradata", "Payload:") are
                // trace chatter; warnings and errors go on to the log handler.
                return level > AV_LOG_WARNING;
            }

            /// Units completed since the last call.
            std::vector<TraceUnit> take_units()
            {
                rethrow_failure();
                return std::exchange(m_units, {});
            }

            [[nodiscard]] std::int64_t element_count() const noexcept { return m_element_count; }

            void rethrow_failure() const
            {
                if (m_failure)
                {
                    std::rethrow_exception(m_failure);
                }
            }

        private:
            std::vector<TraceUnit> m_units;
            std::int64_t m_element_count = 0;
            std::exception_ptr m_failure;
        };

        std::string slice_type_name(const AVCodecID codec_id, const std::int64_t slice_type)
        {
            if (codec_id == AV_CODEC_ID_H264)
            {
                switch (slice_type % 5)
                {
                case 0: return "P";
                case 1: return "B";
                case 2: return "I";
                case 3: return "SP";
                case 4: return "SI";
                default: break;
                }
            }
            else if (codec_id == AV_CODEC_ID_HEVC)
            {
                switch (slice_type)
                {
                case 0: return "B";
                case 1: return "P";
                case 2: return "I";
                default: break;
                }
            }
            else if (codec_id == AV_CODEC_ID_MPEG2VIDEO)
            {
                switch (slice_type)
                {
                case 1: return "I";
                case 2: return "P";
                case 3: return "B";
                default: break;
                }
            }
            return "unknown";
        }

        /// The text of a user-data payload, when it is text: printable ASCII
        /// and whitespace, ignoring trailing NULs.
        std::optional<std::string> payload_text(const std::vector<std::uint8_t>& payload)
        {
            std::string text(payload.begin(), payload.end());
            while (!text.empty() && text.back() == '\0')
            {
                text.pop_back();
            }
            if (text.empty())
            {
                return std::nullopt;
            }
            const bool printable = std::all_of(text.begin(), text.end(),
                                               [](const char character)
                                               {
                                                   const auto byte = static_cast<unsigned char>(character);
                                                   return (byte >= 0x20 && byte < 0x7f) || byte == '\t' ||
                                                          byte == '\n' || byte == '\r';
                                               });
            if (!printable)
            {
                return std::nullopt;
            }
            return text;
        }

        /// The `key=value` pairs after "options: " in an x264 or x265
        /// settings string. A bare flag such as x265's `no-wpp` maps to an
        /// empty value.
        std::map<std::string, std::string> parse_encoder_options(const std::string& settings)
        {
            std::map<std::string, std::string> options;
            const std::string marker = "options: ";
            const std::size_t start = settings.find(marker);
            if (start == std::string::npos)
            {
                return options;
            }

            std::size_t position = start + marker.size();
            while (position < settings.size())
            {
                const std::size_t end = std::min(settings.find(' ', position), settings.size());
                const std::string token = settings.substr(position, end - position);
                if (!token.empty())
                {
                    const std::size_t equals = token.find('=');
                    if (equals == std::string::npos)
                    {
                        options[token] = "";
                    }
                    else
                    {
                        options[token.substr(0, equals)] = token.substr(equals + 1);
                    }
                }
                position = end + 1;
            }
            return options;
        }

        /// A ColorSpec from CICP codes, which the public enums share with
        /// FFmpeg. Absent codes stay unspecified.
        ColorSpec color_from_codes(const std::optional<std::int64_t> primaries,
                                   const std::optional<std::int64_t> transfer,
                                   const std::optional<std::int64_t> matrix)
        {
            ColorSpec color;
            if (primaries.has_value())
            {
                color.primaries = static_cast<ColorPrimaries>(*primaries);
            }
            if (transfer.has_value())
            {
                color.transfer = static_cast<TransferCharacteristic>(*transfer);
            }
            if (matrix.has_value())
            {
                color.matrix = static_cast<ColorMatrix>(*matrix);
            }
            return color;
        }

        /// Turns trace units into HeaderInfo, codec by codec.
        class HeaderAssembler
        {
        public:
            HeaderAssembler(const AVCodecID codec_id, HeaderInfo& info) : m_codec_id(codec_id), m_info(info) {}

            void add(const TraceUnit& unit)
            {
                if (unit.name == "User Data Unregistered")
                {
                    add_user_data(unit);
                    return;
                }

                switch (m_codec_id)
                {
                case AV_CODEC_ID_H264: add_h264(unit); break;
                case AV_CODEC_ID_HEVC: add_hevc(unit); break;
                case AV_CODEC_ID_MPEG2VIDEO: add_mpeg2(unit); break;
                case AV_CODEC_ID_VP9: add_vp9(unit); break;
                case AV_CODEC_ID_AV1: add_av1(unit); break;
                default: break;
                }
            }

            /// Slices, or for VP9 and AV1 frames, recorded so far.
            [[nodiscard]] int coded_units() const noexcept
            {
                return static_cast<int>(m_info.slices.size() + m_info.quantizer_indices.size());
            }

        private:
            void add_h264(const TraceUnit& unit)
            {
                if (unit.name == "Sequence Parameter Set")
                {
                    add_parameter_set("sps", unit.value_or("seq_parameter_set_id", 0), unit);
                    add_vui_color(unit, "matrix_coefficients");
                }
                else if (unit.name == "Picture Parameter Set")
                {
                    const std::int64_t id = unit.value_or("pic_parameter_set_id", 0);
                    add_parameter_set("pps", id, unit);
                    m_initial_qp_by_pps[id] = 26 + unit.value_or("pic_init_qp_minus26", 0);
                }
                else if (unit.name == "Slice Header")
                {
                    add_slice(slice_type_name(m_codec_id, unit.value_or("slice_type", -1)),
                              slice_qp(unit.value("pic_parameter_set_id"), unit.value("slice_qp_delta")));
                }
            }

            void add_hevc(const TraceUnit& unit)
            {
                if (unit.name == "Video Parameter Set")
                {
                    add_parameter_set("vps", unit.value_or("vps_video_parameter_set_id", 0), unit);
                }
                else if (unit.name == "Sequence Parameter Set")
                {
                    add_parameter_set("sps", unit.value_or("sps_seq_parameter_set_id", 0), unit);
                    add_vui_color(unit, "matrix_coefficients");
                }
                else if (unit.name == "Picture Parameter Set")
                {
                    const std::int64_t id = unit.value_or("pps_pic_parameter_set_id", 0);
                    add_parameter_set("pps", id, unit);
                    m_initial_qp_by_pps[id] = 26 + unit.value_or("init_qp_minus26", 0);
                }
                else if (unit.name == "Slice Segment Header" && unit.value_or("dependent_slice_segment_flag", 0) == 0)
                {
                    add_slice(slice_type_name(m_codec_id, unit.value_or("slice_type", -1)),
                              slice_qp(unit.value("slice_pic_parameter_set_id"), unit.value("slice_qp_delta")));
                }
            }

            void add_mpeg2(const TraceUnit& unit)
            {
                if (unit.name == "Sequence Header")
                {
                    add_parameter_set("sequence_header", 0, unit);
                }
                else if (unit.name == "Sequence Extension")
                {
                    add_parameter_set("sequence_extension", 0, unit);
                }
                else if (unit.name == "Sequence Display Extension")
                {
                    add_parameter_set("sequence_display_extension", 0, unit);
                    if (m_info.bitstream_color.is_null() && unit.value("colour_primaries").has_value())
                    {
                        m_info.bitstream_color = color_from_codes(unit.value("colour_primaries"),
                                                                  unit.value("transfer_characteristics"),
                                                                  unit.value("matrix_coefficients"))
                                                     .to_json();
                    }
                }
                else if (unit.name == "Picture Header")
                {
                    m_picture_type = slice_type_name(m_codec_id, unit.value_or("picture_coding_type", 0));
                }
                else if (unit.name == "Slice Header")
                {
                    const std::optional<std::int64_t> code = unit.value("quantiser_scale_code");
                    add_slice(m_picture_type, code.has_value() ? std::optional<int>(static_cast<int>(*code))
                                                               : std::nullopt);
                }
            }

            void add_vp9(const TraceUnit& unit)
            {
                if (unit.name != "Frame")
                {
                    return;
                }
                if (m_info.bitstream_color.is_null() && unit.value("color_space").has_value())
                {
                    m_info.bitstream_color = vp9_color(unit).to_json();
                }
                if (const std::optional<std::int64_t> index = unit.value("base_q_idx"))
                {
                    m_info.quantizer_indices.push_back(static_cast<int>(*index));
                }
            }

            void add_av1(const TraceUnit& unit)
            {
                if (unit.name == "Sequence Header")
                {
                    add_parameter_set("sequence_header", 0, unit);
                    if (m_info.bitstream_color.is_null())
                    {
                        ColorSpec color;
                        if (unit.value_or("color_description_present_flag", 0) == 1)
                        {
                            color = color_from_codes(unit.value("color_primaries"),
                                                     unit.value("transfer_characteristics"),
                                                     unit.value("matrix_coefficients"));
                        }
                        if (unit.value("color_range").has_value())
                        {
                            color.range =
                                unit.value_or("color_range", 0) == 1 ? ColorRange::Full : ColorRange::Limited;
                        }
                        m_info.bitstream_color = color.to_json();
                    }
                }
                else if (unit.name != "Redundant Frame Header")
                {
                    if (const std::optional<std::int64_t> index = unit.value("base_q_idx"))
                    {
                        m_info.quantizer_indices.push_back(static_cast<int>(*index));
                    }
                }
            }

            /// Records a parameter set unless an identical one was already
            /// recorded, since streams repeat them before every keyframe.
            void add_parameter_set(std::string kind, const std::int64_t id, const TraceUnit& unit)
            {
                ParameterSet parameter_set{std::move(kind), static_cast<int>(id), unit.fields()};
                const bool seen = std::any_of(m_info.parameter_sets.begin(), m_info.parameter_sets.end(),
                                              [&](const ParameterSet& existing)
                                              {
                                                  return existing.kind == parameter_set.kind &&
                                                         existing.id == parameter_set.id &&
                                                         existing.fields == parameter_set.fields;
                                              });
                if (!seen)
                {
                    m_info.parameter_sets.push_back(std::move(parameter_set));
                }
            }

            /// The first SPS's VUI color, for H.264 and HEVC, which name the
            /// VUI fields alike.
            void add_vui_color(const TraceUnit& unit, const std::string_view matrix_field)
            {
                if (!m_info.bitstream_color.is_null() || unit.value_or("video_signal_type_present_flag", 0) == 0)
                {
                    return;
                }

                ColorSpec color;
                if (unit.value_or("colour_description_present_flag", 0) == 1)
                {
                    color = color_from_codes(unit.value("colour_primaries"), unit.value("transfer_characteristics"),
                                             unit.value(matrix_field));
                }
                color.range = unit.value_or("video_full_range_flag", 0) == 1 ? ColorRange::Full : ColorRange::Limited;
                if (unit.value_or("chroma_loc_info_present_flag", 0) == 1)
                {
                    // AVChromaLocation numbers the VUI's chroma sample location
                    // types from 1.
                    color.chroma_location =
                        static_cast<ChromaLocation>(unit.value_or("chroma_sample_loc_type_top_field", 0) + 1);
                }
                m_info.bitstream_color = color.to_json();
            }

            /// VP9 codes a color space rather than CICP values.
            static ColorSpec vp9_color(const TraceUnit& unit)
            {
                ColorSpec color;
                switch (unit.value_or("color_space", 0))
                {
                case 1: color.matrix = ColorMatrix::Bt470bg; break;
                case 2: color.matrix = ColorMatrix::Bt709; break;
                case 3: color.matrix = ColorMatrix::Smpte170m; break;
                case 4: color.matrix = ColorMatrix::Smpte240m; break;
                case 5: color.matrix = ColorMatrix::Bt2020Ncl; break;
                case 7: color.matrix = ColorMatrix::Rgb; break;
                default: break;
                }
                if (unit.value("color_range").has_value())
                {
                    color.range = unit.value_or("color_range", 0) == 1 ? ColorRange::Full : ColorRange::Limited;
                }
                else if (color.matrix == ColorMatrix::Rgb)
                {
                    color.range = ColorRange::Full;
                }
                return color;
            }

            [[nodiscard]] std::optional<int> slice_qp(const std::optional<std::int64_t> pps_id,
                                                      const std::optional<std::int64_t> qp_delta) const
            {
                if (!pps_id.has_value() || !qp_delta.has_value())
                {
                    return std::nullopt;
                }
                const auto it = m_initial_qp_by_pps.find(*pps_id);
                if (it == m_initial_qp_by_pps.end())
                {
                    return std::nullopt;
                }
                return static_cast<int>(it->second + *qp_delta);
            }

            void add_slice(std::string slice_type, const std::optional<int> qp)
            {
                SliceInfo slice;
                slice.index = static_cast<int>(m_info.slices.size());
                slice.slice_type = std::move(slice_type);
                slice.qp = qp;
                m_info.slices.push_back(std::move(slice));
            }

            void add_user_data(const TraceUnit& unit)
            {
                if (m_info.embedded_encoder_settings.has_value())
                {
                    return;
                }

                std::vector<std::uint8_t> payload;
                for (const auto& [name, value] : unit.elements)
                {
                    if (name.starts_with("user_data_payload_byte["))
                    {
                        payload.push_back(static_cast<std::uint8_t>(value));
                    }
                }
                std::optional<std::string> text = payload_text(payload);
                if (!text.has_value())
                {
                    return;
                }
                m_info.encoder_settings = parse_encoder_options(*text);
                m_info.embedded_encoder_settings = std::move(text);
                m_info.embedded_encoder_settings_availability = Availability::Present;
            }

            AVCodecID m_codec_id;
            HeaderInfo& m_info;
            std::map<std::int64_t, std::int64_t> m_initial_qp_by_pps;

            /// MPEG-2 codes the picture type once per picture, not per slice.
            std::string m_picture_type = "unknown";
        };

        bool is_interpreted(const AVCodecID codec_id)
        {
            return codec_id == AV_CODEC_ID_H264 || codec_id == AV_CODEC_ID_HEVC ||
                   codec_id == AV_CODEC_ID_MPEG2VIDEO || codec_id == AV_CODEC_ID_VP9 || codec_id == AV_CODEC_ID_AV1;
        }

        const AVStream& select_stream(const AVFormatContext& format, const ReadHeadersOptions& options,
                                      const Source& source)
        {
            if (options.stream_index >= 0)
            {
                if (static_cast<unsigned int>(options.stream_index) >= format.nb_streams)
                {
                    throw ConfigError("no stream " + std::to_string(options.stream_index) + " in " +
                                      source.describe());
                }
                const AVStream& stream = *format.streams[options.stream_index];
                if (stream.codecpar->codec_type != AVMEDIA_TYPE_VIDEO)
                {
                    throw ConfigError("stream " + std::to_string(options.stream_index) + " in " + source.describe() +
                                      " is not video");
                }
                return stream;
            }

            for (unsigned int index = 0; index < format.nb_streams; ++index)
            {
                const AVStream& stream = *format.streams[index];
                if (stream.codecpar->codec_type == AVMEDIA_TYPE_VIDEO &&
                    (stream.disposition & AV_DISPOSITION_ATTACHED_PIC) == 0)
                {
                    return stream;
                }
            }
            throw ConfigError("no video stream in " + source.describe());
        }

        detail::BsfContextPtr allocate_trace_filter(const AVStream& stream)
        {
            const AVBitStreamFilter* filter = av_bsf_get_by_name("trace_headers");
            if (filter == nullptr)
            {
                throw UnsupportedCapability("bitstream filter", "trace_headers", build_info().identity_hash);
            }

            AVBSFContext* allocated = nullptr;
            LL_FF_CHECK(av_bsf_alloc(filter, &allocated));
            detail::BsfContextPtr context(allocated);
            LL_FF_CHECK(avcodec_parameters_copy(context->par_in, stream.codecpar));
            context->time_base_in = stream.time_base;
            return context;
        }
    }

    HeaderInfo read_headers(const Source& source, const ReadHeadersOptions& options)
    {
        if (options.max_slices < 0)
        {
            throw ConfigError("read_headers() max_slices cannot be negative");
        }

        detail::InputContext input(source);
        AVFormatContext& format = *input.get();

        // The raw MPEG video demuxer labels every elementary stream MPEG-1
        // until stream-info detection has seen a sequence extension.
        if (std::strcmp(format.iformat->name, "mpegvideo") == 0)
        {
            input.find_stream_info();
        }
        const AVStream& stream = select_stream(format, options, source);
        const AVCodecID codec_id = stream.codecpar->codec_id;
        const char* codec_name = avcodec_get_name(codec_id);

        HeaderInfo info;
        info.codec_name = codec_name != nullptr ? codec_name : "unknown";
        if (!is_interpreted(codec_id))
        {
            throw UnsupportedCapability("bitstream parser", info.codec_name, build_info().identity_hash,
                                        "read_headers() does not interpret '" + info.codec_name +
                                            "'; it reads H.264, HEVC, MPEG-2, VP9 and AV1");
        }

        for (unsigned int index = 0; index < format.nb_streams; ++index)
        {
            if (static_cast<int>(index) != stream.index)
            {
                format.streams[index]->discard = AVDISCARD_ALL;
            }
        }

        detail::BsfContextPtr filter = allocate_trace_filter(stream);
        TraceCollector collector;
        HeaderAssembler assembler(codec_id, info);

        // The capture has to be open before av_bsf_init, which traces the
        // extradata's parameter sets.
        const detail::ContextLogCapture capture(
            filter.get(), [&collector](const int level, const char* log_format, va_list args)
            { return collector.on_log(level, log_format, args); });
        LL_FF_CHECK(av_bsf_init(filter.get()));

        const auto assemble = [&]
        {
            for (const TraceUnit& unit : collector.take_units())
            {
                assembler.add(unit);
            }
        };
        assemble();

        detail::PacketPtr packet = detail::make_packet();
        int packets_sent = 0;
        while (packets_sent == 0 || assembler.coded_units() < options.max_slices)
        {
            const int status = av_read_frame(&format, packet.get());
            if (status == AVERROR_EOF)
            {
                break;
            }
            LL_FF_CHECK(status);
            if (packet->stream_index != stream.index)
            {
                av_packet_unref(packet.get());
                continue;
            }

            const std::size_t slices_before = info.slices.size();
            const std::int64_t packet_size = packet->size;
            LL_FF_CHECK(av_bsf_send_packet(filter.get(), packet.get()));
            ++packets_sent;
            while (true)
            {
                const int received = av_bsf_receive_packet(filter.get(), packet.get());
                if (received == AVERROR(EAGAIN) || received == AVERROR_EOF)
                {
                    break;
                }
                LL_FF_CHECK(received);
                av_packet_unref(packet.get());
            }
            assemble();

            // A packet is one coded picture, so its size is the slice's size
            // only when it holds a single slice.
            if (info.slices.size() == slices_before + 1)
            {
                info.slices.back().size_bytes = packet_size;
            }
        }

        if (packets_sent > 0 && collector.element_count() == 0)
        {
            throw Error("read_headers() received no bitstream trace from FFmpeg for " + source.describe() +
                        "; its log output may have been redirected by another component in the process");
        }

        if (static_cast<int>(info.slices.size()) > options.max_slices)
        {
            info.slices.resize(static_cast<std::size_t>(options.max_slices));
        }
        if (static_cast<int>(info.quantizer_indices.size()) > options.max_slices)
        {
            info.quantizer_indices.resize(static_cast<std::size_t>(options.max_slices));
        }
        return info;
    }
}
