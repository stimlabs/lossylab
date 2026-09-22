#include "lossylab/io/probe.hpp"

#include "lossylab/core/error.hpp"
#include "lossylab/core/json_io.hpp"
#include "lossylab/core/schema_version.hpp"
#include "lossylab/detail/ff_error.hpp"
#include "lossylab/io/image_container.hpp"
#include "lossylab/io/input_context.hpp"
#include "lossylab/io/jpeg_markers.hpp"

#include <algorithm>
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

        /// `nominal` is the container's declared rate; `average` is measured
        /// from actual frame timing. A gap between them beyond a small
        /// tolerance means the stream is not delivered at a constant rate.
        bool frame_rate_is_variable(const Rational nominal, const Rational average)
        {
            if (!nominal.is_valid() || !average.is_valid() || nominal.num == 0 ||
                average.num == 0)
            {
                return false;
            }

            const double nominal_value = static_cast<double>(nominal.num) / nominal.den;
            const double average_value = static_cast<double>(average.num) / average.den;
            constexpr double tolerance = 0.02;
            return std::abs(nominal_value - average_value) > tolerance * nominal_value;
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
                info.is_variable_frame_rate =
                    frame_rate_is_variable(info.frame_rate, info.average_frame_rate);
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
            info.is_default = (stream.disposition & AV_DISPOSITION_DEFAULT) != 0;
            info.is_dependent = (stream.disposition & AV_DISPOSITION_DEPENDENT) != 0;
            return info;
        }

        TileGrid read_tile_grid(const AVStreamGroup& group)
        {
            const AVStreamGroupTileGrid& grid = *group.params.tile_grid;

            TileGrid info;
            info.id = group.id;
            info.is_primary = (group.disposition & AV_DISPOSITION_DEFAULT) != 0;
            if (const AVDictionaryEntry* title = av_dict_get(group.metadata, "title", nullptr, 0))
            {
                info.title = title->value;
            }
            info.width = grid.width;
            info.height = grid.height;
            info.coded_width = grid.coded_width;
            info.coded_height = grid.coded_height;

            // An offset names its tile by position within the group, not by
            // the stream's index in the file.
            for (unsigned int i = 0; i < grid.nb_tiles; ++i)
            {
                const unsigned int position = grid.offsets[i].idx;
                if (position >= group.nb_streams)
                {
                    continue;
                }
                info.tiles.push_back(TileGrid::Tile{group.streams[position]->index, grid.offsets[i].horizontal,
                                                    grid.offsets[i].vertical});
            }
            return info;
        }

        std::vector<std::string> split_csv(const std::string& text)
        {
            std::vector<std::string> items;
            std::istringstream stream(text);
            std::string item;
            while (std::getline(stream, item, ','))
            {
                if (!item.empty())
                {
                    items.push_back(item);
                }
            }
            return items;
        }

        /// The four-character codes FFmpeg concatenates into its
        /// `compatible_brands` tag, kept verbatim, including brands padded
        /// with spaces such as "qt  ".
        std::vector<std::string> split_brands(const std::string& concatenated)
        {
            constexpr std::size_t brand_length = 4;
            std::vector<std::string> brands;
            for (std::size_t offset = 0; offset + brand_length <= concatenated.size(); offset += brand_length)
            {
                brands.push_back(concatenated.substr(offset, brand_length));
            }
            return brands;
        }

        /// True when `extension` (already lowercased, no dot) is one of the
        /// extensions FFmpeg lists for this demuxer. Demuxers list their
        /// extensions in `AVInputFormat::extensions`; where that field is
        /// unset, FFmpeg's own convention is that `name` doubles as the
        /// extension list (true of, for instance, the mov/mp4 demuxer). A
        /// still-image format read without a container, such as a bare PNG
        /// or JPEG, is matched by a "*_pipe" demuxer instead (e.g. "png_pipe"
        /// for a ".png"), so that suffix is stripped before comparing too.
        bool extension_matches_format(const AVInputFormat& iformat, const std::string& extension)
        {
            const char* list = iformat.extensions != nullptr ? iformat.extensions : iformat.name;
            if (list == nullptr)
            {
                return true; // Nothing to compare against; do not report a mismatch.
            }
            for (const std::string& candidate : split_csv(list))
            {
                if (candidate == extension)
                {
                    return true;
                }

                constexpr std::string_view pipe_suffix = "_pipe";
                if (candidate.size() > pipe_suffix.size() &&
                    candidate.compare(candidate.size() - pipe_suffix.size(), pipe_suffix.size(),
                                      pipe_suffix) == 0 &&
                    candidate.compare(0, candidate.size() - pipe_suffix.size(), extension) == 0)
                {
                    return true;
                }
            }
            return false;
        }

        bool has_brand(const ProbeResult& result, const std::string_view brand)
        {
            return result.major_brand == brand ||
                   std::find(result.compatible_brands.begin(), result.compatible_brands.end(),
                             std::string(brand)) != result.compatible_brands.end();
        }

        /// AVIF and HEIF share the ISOBMFF container with ordinary MP4/MOV
        /// and are told apart by brand.
        bool is_image_item_container(const ProbeResult& result)
        {
            return has_brand(result, "avif") || has_brand(result, "avis") || has_brand(result, "heic") ||
                   has_brand(result, "heix") || has_brand(result, "mif1") || has_brand(result, "msf1");
        }

        ImageContainerInfo webp_container_info(const detail::WebpChunks& chunks)
        {
            ImageContainerInfo info;
            info.has_alpha = chunks.has_alpha;
            info.is_animated = chunks.is_animated;
            info.frame_count = chunks.frame_count;
            info.canvas_width = chunks.canvas_width;
            info.canvas_height = chunks.canvas_height;
            if (chunks.has_lossy && chunks.has_lossless)
            {
                info.compression = "mixed";
            }
            else if (chunks.has_lossless)
            {
                info.compression = "lossless";
            }
            else if (chunks.has_lossy)
            {
                info.compression = "lossy";
            }
            return info;
        }

        /// Fills in WebP/AVIF/HEIF-specific properties for a stream, without
        /// decoding. WebP's come from its RIFF chunks, which say what FFmpeg
        /// does not: lossy or lossless, alpha, and animation. AVIF's and
        /// HEIF's come from what probing already recovered.
        void fill_image_container(const ProbeResult& result, const Source& source, StreamInfo& stream)
        {
            if (result.format_name.find("webp") != std::string::npos)
            {
                if (const std::optional<detail::WebpChunks> chunks = detail::read_webp_chunks(source))
                {
                    stream.image_container = webp_container_info(*chunks);
                }
                return;
            }

            if (!is_image_item_container(result))
            {
                return;
            }

            ImageContainerInfo info;
            info.has_alpha = stream.pixel_format.is_valid() && stream.pixel_format.has_alpha();
            info.is_animated = stream.frame_count.has_value() && *stream.frame_count > 1;
            info.is_still_image = stream.frame_count.has_value() && *stream.frame_count == 1;
            stream.image_container = info;
        }
    }

    json::Value ImageContainerInfo::to_json() const
    {
        return json::object({
            {"has_alpha", has_alpha},
            {"is_animated", is_animated},
            {"is_still_image", json::optional_or_null(is_still_image)},
            {"compression", json::optional_or_null(compression)},
            {"frame_count", json::optional_or_null(frame_count)},
            {"canvas_width", json::optional_or_null(canvas_width)},
            {"canvas_height", json::optional_or_null(canvas_height)},
        });
    }

    json::Value JpegInfo::to_json() const
    {
        json::Array component_values;
        for (const Component& component : components)
        {
            component_values.push_back(json::object({
                {"id", component.id},
                {"horizontal_sampling", component.horizontal_sampling},
                {"vertical_sampling", component.vertical_sampling},
                {"quantization_table", component.quantization_table},
            }));
        }
        json::Array table_values;
        for (const QuantizationTable& table : quantization_tables)
        {
            json::Array values(table.values.begin(), table.values.end());
            table_values.push_back(json::object({
                {"id", table.id},
                {"precision", table.precision},
                {"values", json::array(std::move(values))},
            }));
        }
        json::Array segment_values;
        for (const Segment& segment : segments)
        {
            segment_values.push_back(json::object({
                {"marker", segment.marker},
                {"identifier", segment.identifier},
                {"size_bytes", segment.size_bytes},
            }));
        }
        return json::object({
            {"process", process},
            {"arithmetic_coding", arithmetic_coding},
            {"precision", precision},
            {"components", json::array(std::move(component_values))},
            {"quantization_tables", json::array(std::move(table_values))},
            {"ijg_quality", json::optional_or_null(ijg_quality)},
            {"ijg_quality_exact", ijg_quality_exact},
            {"huffman_tables", huffman_tables},
            {"restart_interval", restart_interval},
            {"scan_count", scan_count},
            {"segments", json::array(std::move(segment_values))},
            {"comment", json::optional_or_null(comment)},
            {"adobe_transform", json::optional_or_null(adobe_transform)},
            {"has_end_of_image", has_end_of_image},
            {"trailing_bytes", trailing_bytes},
        });
    }

    json::Value TileGrid::to_json() const
    {
        json::Array tile_values;
        for (const Tile& tile : tiles)
        {
            tile_values.push_back(json::object({{"stream_index", tile.stream_index}, {"x", tile.x}, {"y", tile.y}}));
        }
        return json::object({
            {"id", id},
            {"is_primary", is_primary},
            {"title", title},
            {"width", width},
            {"height", height},
            {"coded_width", coded_width},
            {"coded_height", coded_height},
            {"tiles", json::array(std::move(tile_values))},
        });
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
            {"is_variable_frame_rate", is_variable_frame_rate},
            {"time_base", time_base.to_json()},
            {"sample_aspect_ratio", sample_aspect_ratio.to_json()},
            {"rotation", json::optional_or_null(rotation)},
            {"frame_count", json::optional_or_null(frame_count)},
            {"duration_us", json::optional_or_null(duration_us)},
            {"bit_rate", json::optional_or_null(bit_rate)},
            {"has_hdr_metadata", has_hdr_metadata},
            {"is_default", is_default},
            {"is_dependent", is_dependent},
            {"image_container", json::optional_or_null(image_container)},
            {"jpeg", json::optional_or_null(jpeg)},
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
            if (stream.type == "video" && stream.is_default)
            {
                return &stream;
            }
        }
        for (const StreamInfo& stream : streams)
        {
            if (stream.type == "video" && !stream.is_dependent)
            {
                return &stream;
            }
        }
        return nullptr;
    }

    const TileGrid* ProbeResult::primary_tile_grid() const noexcept
    {
        for (const TileGrid& grid : tile_grids)
        {
            if (grid.is_primary)
            {
                return &grid;
            }
        }
        return nullptr;
    }

    ProbeResult::AdditionalImages ProbeResult::additional_images() const
    {
        AdditionalImages additional;
        if (!is_image_item_container(*this))
        {
            return additional;
        }

        const StreamInfo* primary = primary_video_stream();
        for (const StreamInfo& stream : streams)
        {
            if (stream.type == "video" && !stream.is_dependent && &stream != primary)
            {
                additional.stream_indices.push_back(stream.index);
            }
        }
        for (const TileGrid& grid : tile_grids)
        {
            if (!grid.is_primary)
            {
                additional.tile_grid_ids.push_back(grid.id);
            }
        }
        return additional;
    }

    json::Value ProbeResult::to_json() const
    {
        return json::object({
            {"schema_version", schema_version},
            {"format", format_name},
            {"format_long_name", format_long_name},
            {"duration_us", json::optional_or_null(duration_us)},
            {"bit_rate", json::optional_or_null(bit_rate)},
            {"size_bytes", json::optional_or_null(size_bytes)},
            {"encoder", json::optional_or_null(encoder_string())},
            {"major_brand", major_brand},
            {"compatible_brands", json::to_array(compatible_brands)},
            {"claimed_extension", claimed_extension},
            {"format_mismatch", format_mismatch},
            {"metadata", json::to_object(metadata)},
            {"streams", json::to_array(streams)},
            {"tile_grids", json::to_array(tile_grids)},
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

        result.claimed_extension = source.claimed_extension();
        if (!result.claimed_extension.empty() && format.iformat != nullptr)
        {
            result.format_mismatch =
                !extension_matches_format(*format.iformat, result.claimed_extension);
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

        for (unsigned int i = 0; i < format.nb_stream_groups; ++i)
        {
            const AVStreamGroup& group = *format.stream_groups[i];
            if (group.type == AV_STREAM_GROUP_PARAMS_TILE_GRID)
            {
                result.tile_grids.push_back(read_tile_grid(group));
            }
        }

        for (StreamInfo& stream : result.streams)
        {
            if (stream.type == "video")
            {
                fill_image_container(result, source, stream);
            }
            // A JPEG file is one stream; motion JPEG inside a video container
            // does not start with a JPEG marker and is left alone.
            if (stream.codec_name == "mjpeg" && result.streams.size() == 1)
            {
                stream.jpeg = detail::read_jpeg_markers(source);
            }
        }

        return result;
    }
}
