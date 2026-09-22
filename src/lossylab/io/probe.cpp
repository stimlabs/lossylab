#include "lossylab/io/probe.hpp"

#include "lossylab/core/error.hpp"
#include "lossylab/core/json_io.hpp"
#include "lossylab/detail/ff_error.hpp"
#include "lossylab/io/input_context.hpp"

#include <cmath>
#include <sstream>

extern "C" {
#include <libavutil/display.h>
#include <libavutil/mastering_display_metadata.h>
#include <libavutil/pixdesc.h>
}

namespace lossylab
{
    namespace
    {
        std::map<std::string, std::string> read_metadata(const AVDictionary* dictionary)
        {
            std::map<std::string, std::string> entries;
            const AVDictionaryEntry* entry = nullptr;
            while ((entry = av_dict_iterate(dictionary, entry)) != nullptr)
            {
                entries.emplace(entry->key, entry->value != nullptr ? entry->value : "");
            }
            return entries;
        }

        std::string media_type_name(const AVMediaType type)
        {
            const char* name = av_get_media_type_string(type);
            return name != nullptr ? name : "unknown";
        }

        std::optional<double> read_rotation(const AVStream& stream)
        {
            const AVPacketSideData* side = av_packet_side_data_get(
                stream.codecpar->coded_side_data, stream.codecpar->nb_coded_side_data,
                AV_PKT_DATA_DISPLAYMATRIX);
            if (side == nullptr || side->size < sizeof(std::int32_t) * 9)
            {
                return std::nullopt;
            }

            const double degrees =
                av_display_rotation_get(reinterpret_cast<const std::int32_t*>(side->data));
            if (std::isnan(degrees))
            {
                return std::nullopt;
            }
            return degrees;
        }

        bool has_hdr_side_data(const AVStream& stream)
        {
            const AVPacketSideData* coded = stream.codecpar->coded_side_data;
            const int count = stream.codecpar->nb_coded_side_data;
            return av_packet_side_data_get(coded, count,
                                           AV_PKT_DATA_MASTERING_DISPLAY_METADATA) != nullptr ||
                   av_packet_side_data_get(coded, count, AV_PKT_DATA_CONTENT_LIGHT_LEVEL) !=
                       nullptr;
        }

        std::optional<std::int64_t> positive_or_none(const std::int64_t value)
        {
            return value > 0 ? std::optional<std::int64_t>(value) : std::nullopt;
        }

        StreamInfo read_stream(const AVStream& stream)
        {
            const AVCodecParameters& params = *stream.codecpar;

            StreamInfo info;
            info.index = stream.index;
            info.type = media_type_name(params.codec_type);

            const char* codec_name = avcodec_get_name(params.codec_id);
            info.codec_name = codec_name != nullptr ? codec_name : "unknown";
            if (const AVCodecDescriptor* descriptor = avcodec_descriptor_get(params.codec_id);
                descriptor != nullptr && descriptor->long_name != nullptr)
            {
                info.codec_long_name = descriptor->long_name;
            }

            if (const char* profile = avcodec_profile_name(params.codec_id, params.profile);
                profile != nullptr)
            {
                info.profile = profile;
            }
            if (params.level != AV_LEVEL_UNKNOWN)
            {
                info.level = params.level;
            }

            info.width = params.width;
            info.height = params.height;
            info.time_base = Rational{stream.time_base.num, stream.time_base.den};
            info.frame_rate = Rational{stream.r_frame_rate.num, stream.r_frame_rate.den};
            info.average_frame_rate = Rational{stream.avg_frame_rate.num, stream.avg_frame_rate.den};

            if (params.sample_aspect_ratio.num != 0)
            {
                info.sample_aspect_ratio =
                    Rational{params.sample_aspect_ratio.num, params.sample_aspect_ratio.den};
            }

            if (params.codec_type == AVMEDIA_TYPE_VIDEO)
            {
                info.pixel_format = PixelFormat::from_raw(params.format);
                info.bit_depth = info.pixel_format.is_valid() ? info.pixel_format.bit_depth() : 0;

                // As tagged, not as encoded: the point of probing is to learn
                // what the file claims, which may be wrong or simply absent.
                info.color.matrix = static_cast<ColorMatrix>(params.color_space);
                info.color.range = static_cast<ColorRange>(params.color_range);
                info.color.primaries = static_cast<ColorPrimaries>(params.color_primaries);
                info.color.transfer = static_cast<TransferCharacteristic>(params.color_trc);
                info.color.chroma_location =
                    static_cast<ChromaLocation>(params.chroma_location);
                info.color_fully_tagged = info.color.is_fully_specified();

                info.rotation = read_rotation(stream);
                info.has_hdr_metadata = has_hdr_side_data(stream);
            }

            info.frame_count = positive_or_none(stream.nb_frames);
            info.bit_rate = positive_or_none(params.bit_rate);
            if (stream.duration != AV_NOPTS_VALUE && stream.duration > 0 &&
                stream.time_base.den != 0)
            {
                info.duration_us = av_rescale_q(stream.duration, stream.time_base,
                                                AVRational{1, 1000000});
            }

            info.metadata = read_metadata(stream.metadata);
            return info;
        }

        std::vector<std::string> split_brands(const std::string& text)
        {
            std::vector<std::string> brands;
            std::istringstream stream(text);
            std::string brand;
            while (std::getline(stream, brand, ','))
            {
                if (!brand.empty())
                {
                    brands.push_back(brand);
                }
            }
            return brands;
        }
    }

    json::Value StreamInfo::to_json() const
    {
        return json::object({
            {"index", index},
            {"type", type},
            {"codec", codec_name},
            {"codec_long_name", codec_long_name},
            {"profile", profile},
            {"level", json::optional_or_null(level)},
            {"width", width},
            {"height", height},
            {"pix_fmt", pixel_format.to_json()},
            {"bit_depth", bit_depth},
            {"color", color.to_json()},
            {"color_fully_tagged", color_fully_tagged},
            {"frame_rate", frame_rate.to_json()},
            {"average_frame_rate", average_frame_rate.to_json()},
            {"time_base", time_base.to_json()},
            {"sample_aspect_ratio", sample_aspect_ratio.to_json()},
            {"rotation", json::optional_or_null(rotation)},
            {"frame_count", json::optional_or_null(frame_count)},
            {"duration_us", json::optional_or_null(duration_us)},
            {"bit_rate", json::optional_or_null(bit_rate)},
            {"has_hdr_metadata", has_hdr_metadata},
            {"metadata", json::to_object(metadata)},
        });
    }

    std::optional<std::string> ProbeResult::encoder_string() const
    {
        if (const auto it = metadata.find("encoder"); it != metadata.end())
        {
            return it->second;
        }
        // Some muxers write it per stream rather than per container.
        for (const StreamInfo& stream : streams)
        {
            if (const auto it = stream.metadata.find("encoder"); it != stream.metadata.end())
            {
                return it->second;
            }
        }
        return std::nullopt;
    }

    const StreamInfo* ProbeResult::primary_video_stream() const noexcept
    {
        for (const StreamInfo& stream : streams)
        {
            if (stream.type == "video")
            {
                return &stream;
            }
        }
        return nullptr;
    }

    json::Value ProbeResult::to_json() const
    {
        return json::object({
            {"format", format_name},
            {"format_long_name", format_long_name},
            {"duration_us", json::optional_or_null(duration_us)},
            {"bit_rate", json::optional_or_null(bit_rate)},
            {"size_bytes", json::optional_or_null(size_bytes)},
            {"encoder", json::optional_or_null(encoder_string())},
            {"major_brand", major_brand},
            {"compatible_brands", json::to_array(compatible_brands)},
            {"metadata", json::to_object(metadata)},
            {"streams", json::to_array(streams)},
        });
    }

    ProbeResult probe(const Source& source)
    {
        detail::InputContext input(source);
        input.find_stream_info();

        const AVFormatContext& format = *input.get();

        ProbeResult result;
        if (format.iformat != nullptr)
        {
            result.format_name = format.iformat->name != nullptr ? format.iformat->name : "";
            result.format_long_name =
                format.iformat->long_name != nullptr ? format.iformat->long_name : "";
        }

        if (format.duration != AV_NOPTS_VALUE && format.duration > 0)
        {
            result.duration_us = format.duration;
        }
        if (format.bit_rate > 0)
        {
            result.bit_rate = format.bit_rate;
        }

        const std::int64_t size = avio_size(format.pb);
        if (size > 0)
        {
            result.size_bytes = size;
        }

        result.metadata = read_metadata(format.metadata);

        // ISOBMFF brands identify the muxer, and platforms that re-encode
        // uploads leave recognizable brand sets behind.
        if (const auto it = result.metadata.find("major_brand"); it != result.metadata.end())
        {
            result.major_brand = it->second;
        }
        if (const auto it = result.metadata.find("compatible_brands");
            it != result.metadata.end())
        {
            result.compatible_brands = split_brands(it->second);
        }

        result.streams.reserve(format.nb_streams);
        for (unsigned i = 0; i < format.nb_streams; ++i)
        {
            result.streams.push_back(read_stream(*format.streams[i]));
        }

        return result;
    }
}
