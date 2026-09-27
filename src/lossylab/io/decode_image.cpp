#include "lossylab/io/decode_image.hpp"

#include "lossylab/convert/convert.hpp"
#include "lossylab/core/error.hpp"
#include "lossylab/detail/ff_error.hpp"
#include "lossylab/detail/ff_ptr.hpp"
#include "lossylab/core/json_io.hpp"
#include "lossylab/env/build_info.hpp"
#include "lossylab/io/icc_profile.hpp"
#include "lossylab/io/input_context.hpp"
#include "lossylab/io/orientation.hpp"

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixdesc.h>
}

#include <algorithm>
#include <cstring>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lossylab
{
    namespace
    {
        /// Finds the video stream carrying the image.
        int find_image_stream(AVFormatContext& format)
        {
            const int index =
                av_find_best_stream(&format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
            if (index < 0)
            {
                throw ConfigError("no image stream found in the source");
            }
            return index;
        }

        detail::CodecContextPtr open_decoder(const AVStream& stream)
        {
            const AVCodec* codec = avcodec_find_decoder(stream.codecpar->codec_id);
            if (codec == nullptr)
            {
                const char* name = avcodec_get_name(stream.codecpar->codec_id);
                throw UnsupportedCapability("decoder", name != nullptr ? name : "unknown",
                                            "this FFmpeg build");
            }

            detail::CodecContextPtr context(LL_FF_ALLOC(avcodec_alloc_context3(codec)));
            LL_FF_CHECK(avcodec_parameters_to_context(context.get(), stream.codecpar));

            // Single-threaded on purpose. Image decoding is fast enough that
            // threads buy little, and pinning the count keeps output identical
            // across machines, which the determinism requirement asks for.
            context->thread_count = 1;

            LL_FF_CHECK(avcodec_open2(context.get(), codec, nullptr));
            return context;
        }

        /// Pulls the first decoded frame out of the stream.
        detail::FramePtr decode_first_frame(AVFormatContext& format, AVCodecContext& decoder,
                                            const int stream_index)
        {
            detail::PacketPtr packet = detail::make_packet();
            detail::FramePtr frame = detail::make_frame();

            while (true)
            {
                const int read = LL_FF_TIMED(av_read_frame(&format, packet.get()));
                if (read == AVERROR_EOF)
                {
                    break;
                }
                LL_FF_CHECK(read);

                if (packet->stream_index != stream_index)
                {
                    av_packet_unref(packet.get());
                    continue;
                }

                const int sent = LL_FF_TIMED(avcodec_send_packet(&decoder, packet.get()));
                av_packet_unref(packet.get());
                if (sent != AVERROR(EAGAIN))
                {
                    LL_FF_CHECK(sent);
                }

                const int received = LL_FF_TIMED(avcodec_receive_frame(&decoder, frame.get()));
                if (received == 0)
                {
                    return frame;
                }
                if (received != AVERROR(EAGAIN))
                {
                    LL_FF_CHECK(received);
                }
            }

            // Flush: a decoder may hold the only frame until told there is no
            // more input.
            LL_FF_CHECK(avcodec_send_packet(&decoder, nullptr));
            const int received = LL_FF_TIMED(avcodec_receive_frame(&decoder, frame.get()));
            if (received == 0)
            {
                return frame;
            }

            throw ConfigError("the source decoded to no frames");
        }

        std::string decoder_name(const AVCodecContext& decoder)
        {
            return decoder.codec->name != nullptr ? decoder.codec->name : "";
        }

        /// A decoded picture, before color handling, with how it was made.
        struct DecodedPicture
        {
            Frame frame;
            std::string decoder_name;

            /// The stream decoded; for a tile grid, its first tile's.
            int stream_index = 0;

            /// The grid assembled, for a tile-grid image.
            std::optional<std::int64_t> tile_grid_id;

            /// The embedded ICC profile, and the EXIF orientation the display
            /// matrix describes, as FFmpeg attached them to the picture.
            std::optional<std::vector<std::uint8_t>> icc_profile;
            std::optional<int> orientation;
        };

        /// Fills in whatever of the ICC profile and orientation `image` does
        /// not have yet from container side data.
        void take_embedded_from(const AVPacketSideData* side_data, const int count, DecodedPicture& image)
        {
            if (const AVPacketSideData* profile = av_packet_side_data_get(side_data, count, AV_PKT_DATA_ICC_PROFILE);
                profile != nullptr && !image.icc_profile.has_value())
            {
                image.icc_profile = std::vector<std::uint8_t>(profile->data, profile->data + profile->size);
            }
            if (const AVPacketSideData* matrix = av_packet_side_data_get(side_data, count, AV_PKT_DATA_DISPLAYMATRIX);
                matrix != nullptr && !image.orientation.has_value())
            {
                image.orientation = detail::orientation_from_display_matrix({matrix->data, matrix->size});
            }
        }

        DecodedPicture decode_single_image(AVFormatContext& format)
        {
            const int stream_index = find_image_stream(format);
            const AVStream& stream = *format.streams[stream_index];

            detail::CodecContextPtr decoder = open_decoder(stream);
            const detail::FramePtr decoded = decode_first_frame(format, *decoder, stream_index);

            Frame frame = Frame::from_av_frame(decoded.get());
            frame.set_time_base(Rational{stream.time_base.num, stream.time_base.den});
            DecodedPicture image{std::move(frame), decoder_name(*decoder), stream_index, std::nullopt, std::nullopt,
                                 std::nullopt};

            // Decoders attach what the bitstream carries (a JPEG's APP2 and
            // EXIF, a PNG's iCCP, a WebP's ICCP) to the frame; the container's
            // side data covers what only the demuxer saw.
            if (const AVFrameSideData* profile = av_frame_get_side_data(decoded.get(), AV_FRAME_DATA_ICC_PROFILE))
            {
                image.icc_profile = std::vector<std::uint8_t>(profile->data, profile->data + profile->size);
            }
            if (const AVFrameSideData* matrix = av_frame_get_side_data(decoded.get(), AV_FRAME_DATA_DISPLAYMATRIX))
            {
                image.orientation = detail::orientation_from_display_matrix({matrix->data, matrix->size});
            }
            take_embedded_from(stream.codecpar->coded_side_data, stream.codecpar->nb_coded_side_data, image);
            return image;
        }

        const AVStreamGroup* find_primary_tile_grid(const AVFormatContext& format)
        {
            for (unsigned int i = 0; i < format.nb_stream_groups; ++i)
            {
                const AVStreamGroup& group = *format.stream_groups[i];
                if (group.type == AV_STREAM_GROUP_PARAMS_TILE_GRID && (group.disposition & AV_DISPOSITION_DEFAULT) != 0)
                {
                    return &group;
                }
            }
            return nullptr;
        }

        /// A tile's rectangle in the assembled image's coordinates, which
        /// start at the grid's crop origin.
        struct TileRect
        {
            const AVStream* stream = nullptr;
            int x = 0;
            int y = 0;
            int width = 0;
            int height = 0;
        };

        /// True when the rectangles leave no pixel of a `width` x `height`
        /// image uncovered. Checked band by band between the rectangles'
        /// top and bottom edges.
        bool tiles_cover(const std::vector<TileRect>& tiles, const int width, const int height)
        {
            std::vector<int> edges = {0, height};
            for (const TileRect& tile : tiles)
            {
                edges.push_back(std::clamp(tile.y, 0, height));
                edges.push_back(std::clamp(tile.y + tile.height, 0, height));
            }
            std::sort(edges.begin(), edges.end());
            edges.erase(std::unique(edges.begin(), edges.end()), edges.end());

            for (std::size_t band = 0; band + 1 < edges.size(); ++band)
            {
                std::vector<std::pair<int, int>> spans;
                for (const TileRect& tile : tiles)
                {
                    if (tile.y <= edges[band] && tile.y + tile.height >= edges[band + 1])
                    {
                        spans.emplace_back(std::max(0, tile.x), std::min(width, tile.x + tile.width));
                    }
                }
                std::sort(spans.begin(), spans.end());
                int covered_to = 0;
                for (const auto& [start, end] : spans)
                {
                    if (start > covered_to)
                    {
                        return false;
                    }
                    covered_to = std::max(covered_to, end);
                }
                if (covered_to < width)
                {
                    return false;
                }
            }
            return true;
        }

        /// Copies a decoded tile into the image at (x, y), clipping whatever
        /// falls outside it. Chroma planes are placed at the position divided
        /// by the format's subsampling.
        void place_tile(Frame& image, const Frame& tile, const int x, const int y)
        {
            const AVPixFmtDescriptor* descriptor =
                av_pix_fmt_desc_get(static_cast<AVPixelFormat>(image.pixel_format().raw()));
            for (int plane_index = 0; plane_index < image.plane_count(); ++plane_index)
            {
                const bool is_chroma = plane_index == 1 || plane_index == 2;
                const int shift_x = is_chroma ? descriptor->log2_chroma_w : 0;
                const int shift_y = is_chroma ? descriptor->log2_chroma_h : 0;
                if (x % (1 << shift_x) != 0 || y % (1 << shift_y) != 0)
                {
                    throw ConfigError("a tile at (" + std::to_string(x) + ", " + std::to_string(y) +
                                      ") is not aligned to the chroma subsampling of " + image.pixel_format().name());
                }

                PlaneView target = image.plane(plane_index);
                const ConstPlaneView source = tile.plane(plane_index);
                const int plane_x = x / (1 << shift_x);
                const int plane_y = y / (1 << shift_y);

                const int source_column = std::max(0, -plane_x);
                const int source_row = std::max(0, -plane_y);
                const int target_column = std::max(0, plane_x);
                const int target_row = std::max(0, plane_y);
                const int columns = std::min(source.width - source_column, target.width - target_column);
                const int rows = std::min(source.height - source_row, target.height - target_row);
                if (columns <= 0 || rows <= 0)
                {
                    continue;
                }

                const std::ptrdiff_t sample_bytes =
                    static_cast<std::ptrdiff_t>(source.bytes_per_sample) * source.components_per_pixel;
                for (int row = 0; row < rows; ++row)
                {
                    std::memcpy(target.row(target_row + row) + target_column * sample_bytes,
                                source.row(source_row + row) + source_column * sample_bytes,
                                static_cast<std::size_t>(columns * sample_bytes));
                }
            }
        }

        /// Decodes one tile's packet on its own: drains the decoder, then
        /// resets it for the next tile.
        detail::FramePtr decode_tile(AVCodecContext& decoder, AVPacket& packet)
        {
            LL_FF_CHECK(avcodec_send_packet(&decoder, &packet));
            LL_FF_CHECK(avcodec_send_packet(&decoder, nullptr));
            detail::FramePtr frame = detail::make_frame();
            const int received = LL_FF_TIMED(avcodec_receive_frame(&decoder, frame.get()));
            avcodec_flush_buffers(&decoder);
            if (received == AVERROR_EOF)
            {
                throw ConfigError("a tile decoded to no picture");
            }
            LL_FF_CHECK(received);
            return frame;
        }

        /// Assembles the primary image of a tile-grid file: every tile decoded
        /// one at a time and copied into the cropped output, so peak memory is
        /// the output plus one tile.
        DecodedPicture decode_tile_grid(AVFormatContext& format, const AVStreamGroup& group)
        {
            const AVStreamGroupTileGrid& grid = *group.params.tile_grid;
            if (grid.nb_tiles == 0 || grid.width <= 0 || grid.height <= 0)
            {
                throw ConfigError("the tile grid declares no tiles or an empty image");
            }

            std::vector<TileRect> tiles;
            for (unsigned int i = 0; i < grid.nb_tiles; ++i)
            {
                if (grid.offsets[i].idx >= group.nb_streams)
                {
                    throw ConfigError("the tile grid names a tile that is not in it");
                }
                const AVStream* stream = group.streams[grid.offsets[i].idx];
                tiles.push_back(TileRect{stream, grid.offsets[i].horizontal - grid.horizontal_offset,
                                         grid.offsets[i].vertical - grid.vertical_offset, stream->codecpar->width,
                                         stream->codecpar->height});
            }
            if (!tiles_cover(tiles, grid.width, grid.height))
            {
                throw NotImplemented("decode_image() for overlay images that leave part of the canvas uncovered");
            }

            // Each tile is one packet of its own stream.
            for (unsigned int i = 0; i < format.nb_streams; ++i)
            {
                const bool in_grid = std::any_of(tiles.begin(), tiles.end(),
                                                 [i](const TileRect& tile) { return tile.stream->index == static_cast<int>(i); });
                format.streams[i]->discard = in_grid ? AVDISCARD_DEFAULT : AVDISCARD_ALL;
            }
            std::map<int, detail::PacketPtr> packets;
            while (true)
            {
                detail::PacketPtr packet = detail::make_packet();
                const int read = LL_FF_TIMED(av_read_frame(&format, packet.get()));
                if (read == AVERROR_EOF)
                {
                    break;
                }
                LL_FF_CHECK(read);
                packets.try_emplace(packet->stream_index, std::move(packet));
            }

            const AVStream& first_stream = *tiles.front().stream;
            detail::CodecContextPtr decoder = open_decoder(first_stream);

            std::optional<Frame> image;
            for (const TileRect& tile : tiles)
            {
                const AVCodecParameters& parameters = *tile.stream->codecpar;
                if (parameters.codec_id != first_stream.codecpar->codec_id)
                {
                    throw ConfigError("the tiles of the grid use different codecs");
                }
                const auto packet = packets.find(tile.stream->index);
                if (packet == packets.end())
                {
                    throw ConfigError("tile stream " + std::to_string(tile.stream->index) + " holds no data");
                }

                const detail::FramePtr decoded = decode_tile(*decoder, *packet->second);
                const Frame tile_frame = Frame::from_av_frame(decoded.get());
                if (tile_frame.width() != tile.width || tile_frame.height() != tile.height)
                {
                    throw ConfigError("tile stream " + std::to_string(tile.stream->index) + " decoded to " +
                                      std::to_string(tile_frame.width()) + "x" + std::to_string(tile_frame.height()) +
                                      " rather than its declared " + std::to_string(tile.width) + "x" +
                                      std::to_string(tile.height));
                }
                if (!image.has_value())
                {
                    image = Frame::allocate(grid.width, grid.height, tile_frame.pixel_format(), tile_frame.color());
                    image->set_time_base(Rational{first_stream.time_base.num, first_stream.time_base.den});
                }
                else if (tile_frame.pixel_format() != image->pixel_format())
                {
                    throw ConfigError("the tiles of the grid decode to different pixel formats");
                }
                place_tile(*image, tile_frame, tile.x, tile.y);
            }

            DecodedPicture decoded{std::move(*image), decoder_name(*decoder), first_stream.index, group.id,
                                   std::nullopt, std::nullopt};
            take_embedded_from(grid.coded_side_data, grid.nb_coded_side_data, decoded);
            return decoded;
        }

        /// Fills the chroma siting a codec fixes but FFmpeg's decoder leaves
        /// unspecified: lossy WebP's centered chroma (VP8, and libwebp's own
        /// RGB conversions). Returns the color and records the change.
        ColorSpec color_implied_by_codec(const ColorSpec& tagged, const std::string& decoder_name,
                                         const PixelFormat& pixel_format, ConversionList& conversions)
        {
            ColorSpec color = tagged;
            const bool subsampled = !pixel_format.is_rgb() && !pixel_format.is_gray() &&
                                    pixel_format.subsampling() != Subsampling::Yuv444;
            if ((decoder_name == "webp" || decoder_name == "libwebp") && subsampled &&
                color.chroma_location == ChromaLocation::Unspecified)
            {
                color.chroma_location = ChromaLocation::Center;
                conversions.push_back(ConversionEvent{"color_tags", tagged.describe(), color.describe(),
                                                      ConversionCause::Requested, "codec_implied_color"});
            }
            return color;
        }

        /// Fills the primaries and transfer a file left unspecified from its
        /// ICC profile, when the profile matches a pair the tags can name.
        /// Returns the color and records the change.
        ColorSpec color_with_icc_profile(const ColorSpec& tagged, const std::optional<IccProfileInfo>& profile,
                                         ConversionList& conversions)
        {
            ColorSpec color = tagged;
            if (!profile.has_value() || !profile->is_expressible_as_tags())
            {
                return color;
            }
            if (color.primaries == ColorPrimaries::Unspecified)
            {
                color.primaries = *profile->primaries;
            }
            if (color.transfer == TransferCharacteristic::Unspecified)
            {
                color.transfer = *profile->transfer;
            }
            if (color != tagged)
            {
                conversions.push_back(ConversionEvent{"color_tags", tagged.describe(), color.describe(),
                                                      ConversionCause::Requested, "icc_profile"});
            }
            return color;
        }

        TileGrid& find_tile_grid(ProbeResult& probed, const std::int64_t id)
        {
            const auto grid = std::find_if(probed.tile_grids.begin(), probed.tile_grids.end(),
                                           [id](const TileGrid& candidate) { return candidate.id == id; });
            if (grid == probed.tile_grids.end())
            {
                throw ConfigError("probe found no tile grid " + std::to_string(id));
            }
            return *grid;
        }

        StreamInfo& find_stream(ProbeResult& probed, const int index)
        {
            const auto stream = std::find_if(probed.streams.begin(), probed.streams.end(),
                                             [index](const StreamInfo& candidate) { return candidate.index == index; });
            if (stream == probed.streams.end())
            {
                throw ConfigError("probe found no stream " + std::to_string(index));
            }
            return *stream;
        }

        /// Makes the probe and the decoded picture agree on the picture's
        /// orientation and ICC profile. The orientation the decoder attached
        /// wins over the file's metadata; the profile the file's metadata
        /// holds wins over the decoder's, since probe's reading of it also
        /// lists its structural problems. Either fills in what the other left
        /// out, and a format probe could not read becomes `Present` or
        /// `NotPresent`. Returns the ICC profile to decode by.
        std::optional<IccProfile> reconcile_embedded(DecodedPicture& picture, ProbeResult& probed,
                                                     const detail::ProbedIccProfiles& probed_profiles)
        {
            std::optional<IccProfile> decoded_profile;
            if (picture.icc_profile.has_value())
            {
                decoded_profile = IccProfile::from_bytes(*picture.icc_profile);
            }

            // The profile probe described, with its bytes; else the decoder's.
            const auto chosen_profile = [&decoded_profile](const std::optional<IccProfileInfo>& probed_info,
                                                           const std::vector<std::uint8_t>* probed_bytes)
                -> std::optional<IccProfile>
            {
                if (probed_info.has_value() && probed_bytes != nullptr)
                {
                    return IccProfile{*probed_bytes, *probed_info};
                }
                return decoded_profile;
            };

            if (picture.tile_grid_id.has_value())
            {
                TileGrid& grid = find_tile_grid(probed, *picture.tile_grid_id);
                if (picture.orientation.has_value())
                {
                    grid.orientation = picture.orientation;
                }
                picture.orientation = grid.orientation;

                const auto bytes = probed_profiles.by_tile_grid.find(grid.id);
                std::optional<IccProfile> profile = chosen_profile(
                    grid.icc_profile, bytes != probed_profiles.by_tile_grid.end() ? &bytes->second : nullptr);
                grid.icc_profile = profile.has_value() ? std::optional(profile->info) : std::nullopt;
                return profile;
            }

            StreamInfo& stream = find_stream(probed, picture.stream_index);
            if (picture.orientation.has_value())
            {
                if (stream.orientation != picture.orientation)
                {
                    stream.orientation = picture.orientation;
                    stream.orientation_source = "decoder";
                }
                stream.orientation_availability = Availability::Present;
            }
            else if (stream.orientation.has_value())
            {
                picture.orientation = stream.orientation;
            }
            else if (picture.decoder_name != "libjxl")
            {
                // libjxl turns the image upright and FFmpeg removes the
                // orientation it applied, so for JPEG XL nothing is known.
                stream.orientation_availability = Availability::NotPresent;
            }

            const auto bytes = probed_profiles.by_stream.find(stream.index);
            std::optional<IccProfile> profile = chosen_profile(
                stream.icc_profile, bytes != probed_profiles.by_stream.end() ? &bytes->second : nullptr);
            if (profile.has_value())
            {
                stream.icc_profile = profile->info;
                stream.icc_profile_availability = Availability::Present;
                stream.icc_matches_tagged_color = profile->info.agrees_with(stream.color);
            }
            else
            {
                stream.icc_profile.reset();
                stream.icc_profile_availability = Availability::NotPresent;
            }
            return profile;
        }

    }

    std::string to_string(const OrientationHandling handling)
    {
        switch (handling)
        {
        case OrientationHandling::Report: return "report";
        case OrientationHandling::Apply: return "apply";
        }
        return "report";
    }

    OrientationHandling orientation_handling_from_string(const std::string_view name)
    {
        if (name == "report")
        {
            return OrientationHandling::Report;
        }
        if (name == "apply")
        {
            return OrientationHandling::Apply;
        }
        throw ConfigError("unknown orientation handling '" + std::string(name) + "'");
    }

    const StreamInfo& DecodedImage::stream() const
    {
        const int index = evidence().stream_index;
        for (const StreamInfo& candidate : probe.streams)
        {
            if (candidate.index == index)
            {
                return candidate;
            }
        }
        throw ConfigError("the probe holds no stream " + std::to_string(index));
    }

    const TileGrid* DecodedImage::tile_grid() const
    {
        const std::optional<std::int64_t>& id = evidence().tile_grid_id;
        if (!id.has_value())
        {
            return nullptr;
        }
        for (const TileGrid& candidate : probe.tile_grids)
        {
            if (candidate.id == *id)
            {
                return &candidate;
            }
        }
        return nullptr;
    }

    ProcessingRecord DecodedImage::processing_record() const
    {
        ProcessingRecord history = ProcessingRecord::for_this_build();
        history.set_origin(probe);
        history.append(record, configuration);
        return history;
    }

    json::Value DecodedImage::to_json() const
    {
        return json::object({
            {"probe", probe.to_json()},
            {"frame", frame.describe().to_json()},
            {"record", record.to_json()},
            {"configuration", reflect::to_json(configuration)},
        });
    }

    DecodedImage decode_image(const Source& source, const DecodeImageOptions& options)
    {
        const detail::StageClock clock;

        detail::InputContext input(source);
        input.find_stream_info();
        detail::ProbedIccProfiles probed_profiles;
        ProbeResult probed = detail::probe_input(input, source, &probed_profiles);

        AVFormatContext& format = *input.get();
        const AVStreamGroup* tile_grid = find_primary_tile_grid(format);
        DecodedPicture decoded = tile_grid != nullptr ? decode_tile_grid(format, *tile_grid)
                                                      : decode_single_image(format);

        const std::optional<IccProfile> embedded_profile = reconcile_embedded(decoded, probed, probed_profiles);
        const std::optional<IccProfileInfo> icc_profile =
            embedded_profile.has_value() ? std::optional(embedded_profile->info) : std::nullopt;
        Frame frame = std::move(decoded.frame);
        if (embedded_profile.has_value())
        {
            frame.set_icc_profile(embedded_profile->bytes);
        }
        else
        {
            frame.clear_icc_profile();
        }
        frame.set_orientation(decoded.decoder_name == "libjxl" ? std::nullopt : decoded.orientation);
        frame.set_sample_aspect_ratio(find_stream(probed, decoded.stream_index).sample_aspect_ratio);

        StageRecord record;
        record.implementation = decoded.decoder_name;
        record.transform = CoordinateTransform::identity();

        // A file that tags no color is not an error, but what stands in for
        // the tags is a decision, so it is recorded as one rather than quietly
        // applied: first the embedded ICC profile, when it names a pair the
        // tags can express, then the caller's assumption.
        const ColorSpec tagged = frame.color();
        const ColorSpec with_codec = color_implied_by_codec(tagged, decoded.decoder_name, frame.pixel_format(),
                                                            record.conversions);
        const ColorSpec with_profile = color_with_icc_profile(with_codec, icc_profile, record.conversions);
        ColorSpec resolved = with_profile;
        if (!with_profile.is_fully_specified())
        {
            resolved = with_profile.with_defaults_from(options.assumed_color);
            record.conversions.push_back(ConversionEvent{
                "color_tags", with_profile.describe(), resolved.describe(),
                ConversionCause::Requested, "assumed_color"});
        }
        if (resolved != tagged)
        {
            frame.set_color(resolved);
            frame.sync_color_to_av_frame();
        }
        record.input = frame.describe();

        // libjxl turns a JPEG XL image upright itself and FFmpeg removes the
        // orientation it applied, so there is nothing left to apply.
        std::string orientation_handling = "reported";
        if (decoded.decoder_name == "libjxl")
        {
            orientation_handling = "applied_by_decoder";
        }
        else if (options.orientation == OrientationHandling::Apply && decoded.orientation.has_value())
        {
            const int stored_width = frame.width();
            const int stored_height = frame.height();
            frame = detail::apply_orientation(frame, *decoded.orientation, options.strict, record.conversions);
            record.transform = CoordinateTransform::orientation(*decoded.orientation, stored_width, stored_height);
            orientation_handling = "applied";
        }

        record.output = frame.describe();
        record.evidence = DecodeImageEvidence{source.sha256(), decoded.stream_index, decoded.tile_grid_id,
                                              orientation_handling};

        // An explicit target means one conversion, run through the same code
        // path everything else uses, so its record is the same shape.
        if (options.pixel_format.has_value() || options.color.has_value())
        {
            ConvertOptions convert_options;
            convert_options.pixel_format =
                options.pixel_format.value_or(frame.pixel_format());
            convert_options.color = options.color.value_or(frame.color());
            convert_options.strict = options.strict;

            FrameResult converted = convert(frame, convert_options);

            record.conversions.insert(record.conversions.end(),
                                      converted.record.conversions.begin(),
                                      converted.record.conversions.end());
            record.output = converted.frame.describe();
            record.duration_ms = clock.duration_ms();
            record.ffmpeg_duration_ms = clock.ffmpeg_duration_ms();
            return DecodedImage{std::move(probed), std::move(converted.frame), std::move(record), options};
        }

        record.duration_ms = clock.duration_ms();
        record.ffmpeg_duration_ms = clock.ffmpeg_duration_ms();
        return DecodedImage{std::move(probed), std::move(frame), std::move(record), options};
    }
}
