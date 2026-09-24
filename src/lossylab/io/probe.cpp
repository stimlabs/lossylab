#include "lossylab/io/probe.hpp"

#include "lossylab/core/error.hpp"
#include "lossylab/core/json_io.hpp"
#include "lossylab/core/schema_version.hpp"
#include "lossylab/detail/ff_error.hpp"
#include "lossylab/io/image_container.hpp"
#include "lossylab/io/input_context.hpp"
#include "lossylab/io/jpeg_markers.hpp"
#include "lossylab/io/orientation.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <span>
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

        std::optional<std::span<const std::uint8_t>> find_side_data(const AVPacketSideData* side_data, const int count,
                                                                    const AVPacketSideDataType type)
        {
            const AVPacketSideData* found = av_packet_side_data_get(side_data, count, type);
            if (found == nullptr)
            {
                return std::nullopt;
            }
            return std::span<const std::uint8_t>(found->data, found->size);
        }

        /// The ICC profile and orientation in a stream's or a grid's side
        /// data, as FFmpeg's ISOBMFF demuxer exports them.
        struct SideDataEmbedded
        {
            std::optional<IccProfileInfo> icc_profile;
            std::optional<int> orientation;
        };

        SideDataEmbedded read_side_data_embedded(const AVPacketSideData* side_data, const int count)
        {
            SideDataEmbedded embedded;
            if (const auto profile = find_side_data(side_data, count, AV_PKT_DATA_ICC_PROFILE))
            {
                embedded.icc_profile = describe_icc_profile(*profile);
            }
            if (const auto matrix = find_side_data(side_data, count, AV_PKT_DATA_DISPLAYMATRIX))
            {
                embedded.orientation = detail::orientation_from_display_matrix(*matrix);
            }
            return embedded;
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
                const SideDataEmbedded embedded =
                    read_side_data_embedded(params.coded_side_data, params.nb_coded_side_data);
                if (embedded.icc_profile.has_value())
                {
                    info.icc_profile = embedded.icc_profile;
                    info.icc_profile_availability = Availability::Present;
                }
                if (embedded.orientation.has_value())
                {
                    info.orientation = embedded.orientation;
                    info.orientation_source = "display_matrix";
                    info.orientation_availability = Availability::Present;
                }
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
            info.crop_x = grid.horizontal_offset;
            info.crop_y = grid.vertical_offset;

            const SideDataEmbedded embedded = read_side_data_embedded(grid.coded_side_data, grid.nb_coded_side_data);
            info.orientation = embedded.orientation;
            info.icc_profile = embedded.icc_profile;

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

        /// True when `extension` (already lowercased, no dot) is one a file
        /// holding a still image of `codec_name` is commonly given. A codec
        /// not listed here is expected to use its own name, as "png" does.
        bool codec_uses_extension(const std::string& codec_name, const std::string& extension)
        {
            struct CodecExtension
            {
                std::string_view codec_name;
                std::string_view extension;
            };
            constexpr std::array known_extensions = {
                CodecExtension{"mjpeg", "jpg"},    CodecExtension{"mjpeg", "jpeg"},
                CodecExtension{"mjpeg", "jpe"},    CodecExtension{"mjpeg", "jfif"},
                CodecExtension{"mjpeg", "jif"},    CodecExtension{"tiff", "tif"},
                CodecExtension{"tiff", "tiff"},    CodecExtension{"jpeg2000", "jp2"},
                CodecExtension{"jpeg2000", "j2k"}, CodecExtension{"jpeg2000", "jpx"},
                CodecExtension{"jpegxl", "jxl"},
            };

            bool codec_is_listed = false;
            for (const CodecExtension& known : known_extensions)
            {
                if (known.codec_name == codec_name)
                {
                    codec_is_listed = true;
                    if (known.extension == extension)
                    {
                        return true;
                    }
                }
            }
            return !codec_is_listed && codec_name == extension;
        }

        /// True when `extension` (already lowercased, no dot) is one of the
        /// extensions FFmpeg lists for this demuxer. Demuxers list their
        /// extensions in `AVInputFormat::extensions`; where that field is
        /// unset, FFmpeg's own convention is that `name` doubles as the
        /// extension list (true of, for instance, the mov/mp4 demuxer). A
        /// still-image format read without a container, such as a bare PNG
        /// or JPEG, is matched by a "*_pipe" demuxer instead (e.g. "png_pipe"
        /// for a ".png"), so that suffix is stripped before comparing too.
        /// A demuxer with no extension list, such as "image2" or the "*_pipe"
        /// ones, says nothing reliable about extensions, so the codec of
        /// `primary_video` decides when the name does not match.
        bool extension_matches_format(const AVInputFormat& iformat, const std::string& extension,
                                      const StreamInfo* primary_video)
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

            if (iformat.extensions == nullptr && primary_video != nullptr)
            {
                return codec_uses_extension(primary_video->codec_name, extension);
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
            info.has_xmp = chunks.has_xmp;
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
        void fill_image_container(const ProbeResult& result, const std::optional<detail::WebpChunks>& webp_chunks,
                                  StreamInfo& stream)
        {
            if (result.format_name.find("webp") != std::string::npos)
            {
                if (webp_chunks.has_value())
                {
                    stream.image_container = webp_container_info(*webp_chunks);
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

        /// The raw ICC profile and EXIF block a format walker found.
        struct WalkedEmbedded
        {
            std::optional<std::vector<std::uint8_t>> icc_profile;
            std::vector<std::string> icc_problems;
            std::optional<std::vector<std::uint8_t>> exif;
        };

        /// Interprets what a walker found. The walker read every place the
        /// format keeps these, so what it did not find is not there.
        void apply_walked_embedded(const WalkedEmbedded& embedded, StreamInfo& stream)
        {
            if (embedded.icc_profile.has_value())
            {
                IccProfileInfo info = describe_icc_profile(*embedded.icc_profile);
                info.problems.insert(info.problems.begin(), embedded.icc_problems.begin(),
                                     embedded.icc_problems.end());
                stream.icc_profile = std::move(info);
                stream.icc_profile_availability = Availability::Present;
            }
            const std::optional<int> orientation =
                embedded.exif.has_value() ? detail::exif_orientation(*embedded.exif) : std::nullopt;
            if (orientation.has_value())
            {
                stream.orientation = orientation;
                stream.orientation_source = "exif";
                stream.orientation_availability = Availability::Present;
            }
        }

        /// A still image FFmpeg reads with a bare image parser rather than
        /// a container demuxer: the demuxer exports no side data, so what
        /// the file embeds is known only after decoding.
        bool is_bare_image_format(const std::string& format_name)
        {
            return format_name == "image2" || format_name.ends_with("_pipe") || format_name == "jpegxl_anim";
        }

    }

    json::Value ImageContainerInfo::to_json() const
    {
        return reflect::to_json(*this);
    }

    ImageContainerInfo ImageContainerInfo::from_json(const json::Value& value)
    {
        return reflect::from_json<ImageContainerInfo>(value);
    }

    json::Value JpegInfo::to_json() const
    {
        return reflect::to_json(*this);
    }

    JpegInfo JpegInfo::from_json(const json::Value& value)
    {
        return reflect::from_json<JpegInfo>(value);
    }

    json::Value TileGrid::to_json() const
    {
        return reflect::to_json(*this);
    }

    TileGrid TileGrid::from_json(const json::Value& value)
    {
        return reflect::from_json<TileGrid>(value);
    }

    json::Value StreamInfo::to_json() const
    {
        return reflect::to_json(*this);
    }

    StreamInfo StreamInfo::from_json(const json::Value& value)
    {
        return reflect::from_json<StreamInfo>(value);
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
        json::Value value = json::object({{"schema_version", schema_version}});
        value.update(reflect::to_json(*this));
        value["encoder"] = json::optional_or_null(encoder_string());
        return value;
    }

    ProbeResult ProbeResult::from_json(const json::Value& value)
    {
        return reflect::from_json<ProbeResult>(value);
    }

    ProbeResult probe(const Source& source)
    {
        detail::InputContext input(source);
        input.find_stream_info();
        return detail::probe_input(input, source);
    }

    ProbeResult detail::probe_input(const InputContext& input, const Source& source, ProbedIccProfiles* icc_profiles)
    {
        const AVFormatContext& format = *input.get();

        ProbeResult result;
        if (format.iformat != nullptr)
        {
            result.format_name = format.iformat->name != nullptr ? format.iformat->name : "";
            result.format_long_name =
                format.iformat->long_name != nullptr ? format.iformat->long_name : "";
        }

        result.claimed_extension = source.claimed_extension();

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

        if (!result.claimed_extension.empty() && format.iformat != nullptr)
        {
            result.format_mismatch = !extension_matches_format(*format.iformat, result.claimed_extension,
                                                               result.primary_video_stream());
        }

        for (unsigned int i = 0; i < format.nb_stream_groups; ++i)
        {
            const AVStreamGroup& group = *format.stream_groups[i];
            if (group.type == AV_STREAM_GROUP_PARAMS_TILE_GRID)
            {
                result.tile_grids.push_back(read_tile_grid(group));
                const AVStreamGroupTileGrid& grid = *group.params.tile_grid;
                if (const auto profile = find_side_data(grid.coded_side_data, grid.nb_coded_side_data,
                                                        AV_PKT_DATA_ICC_PROFILE);
                    profile.has_value() && icc_profiles != nullptr)
                {
                    icc_profiles->by_tile_grid[group.id].assign(profile->begin(), profile->end());
                }
            }
        }
        if (icc_profiles != nullptr)
        {
            for (unsigned int i = 0; i < format.nb_streams; ++i)
            {
                const AVCodecParameters& params = *format.streams[i]->codecpar;
                if (const auto profile =
                        find_side_data(params.coded_side_data, params.nb_coded_side_data, AV_PKT_DATA_ICC_PROFILE))
                {
                    icc_profiles->by_stream[format.streams[i]->index].assign(profile->begin(), profile->end());
                }
            }
        }

        // What a format walker found replaces what the demuxer exported.
        const auto apply_walked = [icc_profiles](const WalkedEmbedded& embedded, StreamInfo& stream)
        {
            apply_walked_embedded(embedded, stream);
            if (icc_profiles != nullptr && embedded.icc_profile.has_value())
            {
                icc_profiles->by_stream[stream.index] = *embedded.icc_profile;
            }
        };

        const bool is_webp = result.format_name.find("webp") != std::string::npos;
        const std::optional<detail::WebpChunks> webp_chunks =
            is_webp ? detail::read_webp_chunks(source) : std::nullopt;
        const bool single_stream = result.streams.size() == 1;

        for (StreamInfo& stream : result.streams)
        {
            if (stream.type != "video")
            {
                continue;
            }
            fill_image_container(result, webp_chunks, stream);

            // A JPEG or PNG file is one stream; motion JPEG inside a video
            // container does not start with a JPEG marker and is left alone.
            if (stream.codec_name == "mjpeg" && single_stream)
            {
                if (std::optional<detail::JpegMarkers> markers = detail::read_jpeg_markers(source))
                {
                    stream.jpeg = std::move(markers->info);
                    WalkedEmbedded embedded{std::nullopt, std::move(markers->icc_problems), std::move(markers->exif)};
                    if (!markers->icc_profile.empty() || !embedded.icc_problems.empty())
                    {
                        embedded.icc_profile = std::move(markers->icc_profile);
                    }
                    apply_walked(embedded, stream);
                }
            }
            else if (webp_chunks.has_value())
            {
                apply_walked({webp_chunks->icc_profile, webp_chunks->icc_problems, webp_chunks->exif}, stream);
            }
            else if ((stream.codec_name == "png" || stream.codec_name == "apng") && single_stream)
            {
                if (const std::optional<detail::PngChunks> chunks = detail::read_png_chunks(source))
                {
                    apply_walked({chunks->icc_profile, chunks->icc_problems, chunks->exif}, stream);
                }
            }
            else if (is_bare_image_format(result.format_name))
            {
                stream.icc_profile_availability = Availability::NotSupportedByBuild;
                stream.orientation_availability = Availability::NotSupportedByBuild;
            }

            if (is_image_item_container(result) && stream.orientation.has_value())
            {
                stream.orientation_source = "irot_imir";
            }
            if (stream.icc_profile.has_value())
            {
                stream.icc_matches_tagged_color = stream.icc_profile->agrees_with(stream.color);
            }
        }

        return result;
    }
}
