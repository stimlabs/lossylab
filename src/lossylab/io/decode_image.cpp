#include "lossylab/io/decode_image.hpp"

#include "lossylab/convert/convert.hpp"
#include "lossylab/core/error.hpp"
#include "lossylab/detail/ff_error.hpp"
#include "lossylab/detail/ff_ptr.hpp"
#include "lossylab/io/input_context.hpp"

extern "C" {
#include <libavutil/pixdesc.h>
}

#include <algorithm>
#include <chrono>
#include <cstring>
#include <map>
#include <optional>
#include <string>
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
                const int read = av_read_frame(&format, packet.get());
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

                const int sent = avcodec_send_packet(&decoder, packet.get());
                av_packet_unref(packet.get());
                if (sent != AVERROR(EAGAIN))
                {
                    LL_FF_CHECK(sent);
                }

                const int received = avcodec_receive_frame(&decoder, frame.get());
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
            const int received = avcodec_receive_frame(&decoder, frame.get());
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
        struct DecodedImage
        {
            Frame frame;
            std::string decoder_name;

            /// Describes the grid for a tile-grid image; null otherwise.
            json::Value tile_grid;
        };

        DecodedImage decode_single_image(AVFormatContext& format)
        {
            const int stream_index = find_image_stream(format);
            const AVStream& stream = *format.streams[stream_index];

            detail::CodecContextPtr decoder = open_decoder(stream);
            const detail::FramePtr decoded = decode_first_frame(format, *decoder, stream_index);

            Frame frame = Frame::from_av_frame(decoded.get());
            frame.set_time_base(Rational{stream.time_base.num, stream.time_base.den});
            return DecodedImage{std::move(frame), decoder_name(*decoder), json::Value()};
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
            const int received = avcodec_receive_frame(&decoder, frame.get());
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
        DecodedImage decode_tile_grid(AVFormatContext& format, const AVStreamGroup& group)
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
                const int read = av_read_frame(&format, packet.get());
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

            json::Value description = json::object({
                {"id", group.id},
                {"tiles", grid.nb_tiles},
                {"tile_width", tiles.front().width},
                {"tile_height", tiles.front().height},
                {"coded_width", grid.coded_width},
                {"coded_height", grid.coded_height},
                {"crop", json::object({{"x", grid.horizontal_offset},
                                       {"y", grid.vertical_offset},
                                       {"width", grid.width},
                                       {"height", grid.height}})},
            });
            return DecodedImage{std::move(*image), decoder_name(*decoder), std::move(description)};
        }
    }

    FrameResult decode_image(const Source& source, const DecodeImageOptions& options)
    {
        const auto started = std::chrono::steady_clock::now();

        detail::InputContext input(source);
        input.find_stream_info();

        AVFormatContext& format = *input.get();
        const AVStreamGroup* tile_grid = find_primary_tile_grid(format);
        DecodedImage decoded = tile_grid != nullptr ? decode_tile_grid(format, *tile_grid)
                                                    : decode_single_image(format);
        Frame frame = std::move(decoded.frame);

        StageRecord record;
        record.kind = StageKind::Decode;
        record.implementation = decoded.decoder_name;
        record.transform = CoordinateTransform::identity();
        record.input = frame.describe();

        // A file that tags no color is not an error, but the assumption made on
        // its behalf is a decision, so it is recorded as one rather than
        // quietly applied.
        const ColorSpec tagged = frame.color();
        if (!tagged.is_fully_specified())
        {
            const ColorSpec assumed = tagged.with_defaults_from(options.assumed_color);
            frame.set_color(assumed);
            frame.sync_color_to_av_frame();
            record.conversions.push_back(ConversionEvent{
                "color_tags", tagged.describe(), assumed.describe(),
                ConversionCause::Requested, "assumed_color"});
        }

        record.input = frame.describe();
        record.output = record.input;
        record.params = json::object({
            {"source", source.describe()},
            {"codec", record.implementation},
            {"tagged_color", tagged.to_json()},
            {"color_fully_tagged", tagged.is_fully_specified()},
            {"assumed_color", options.assumed_color.to_json()},
            {"tile_grid", decoded.tile_grid},
        });

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
            record.params["converted"] = converted.record.params;
            record.duration_ms = std::chrono::duration<double, std::milli>(
                                     std::chrono::steady_clock::now() - started)
                                     .count();
            return FrameResult{std::move(converted.frame), std::move(record)};
        }

        record.duration_ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started)
                .count();
        return FrameResult{std::move(frame), std::move(record)};
    }
}
