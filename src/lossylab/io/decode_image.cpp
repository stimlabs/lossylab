#include "lossylab/io/decode_image.hpp"

#include "lossylab/convert/convert.hpp"
#include "lossylab/core/error.hpp"
#include "lossylab/detail/ff_error.hpp"
#include "lossylab/detail/ff_ptr.hpp"
#include "lossylab/io/input_context.hpp"

#include <chrono>

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
    }

    FrameResult decode_image(const Source& source, const DecodeImageOptions& options)
    {
        const auto started = std::chrono::steady_clock::now();

        detail::InputContext input(source);
        input.find_stream_info();

        AVFormatContext& format = *input.get();
        const int stream_index = find_image_stream(format);
        const AVStream& stream = *format.streams[stream_index];

        detail::CodecContextPtr decoder = open_decoder(stream);
        const detail::FramePtr decoded =
            decode_first_frame(format, *decoder, stream_index);

        Frame frame = Frame::from_av_frame(decoded.get());
        frame.set_time_base(Rational{stream.time_base.num, stream.time_base.den});

        StageRecord record;
        record.kind = StageKind::Decode;
        record.implementation = decoder->codec->name != nullptr ? decoder->codec->name : "";
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
