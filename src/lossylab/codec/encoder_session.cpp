#include "encoder_session.hpp"

#include "lossylab/core/error.hpp"
#include "lossylab/detail/ff_error.hpp"
#include "lossylab/env/build_info.hpp"

extern "C" {
#include <libavcodec/exif.h>
#include <libavutil/avutil.h>
#include <libavutil/display.h>
#include <libavutil/intreadwrite.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
}

#include <algorithm>
#include <cstring>
#include <limits>

namespace lossylab::detail
{
    namespace
    {
        PictureType picture_type_of(const int av_picture_type)
        {
            switch (av_picture_type)
            {
            case AV_PICTURE_TYPE_I: return PictureType::I;
            case AV_PICTURE_TYPE_P: return PictureType::P;
            case AV_PICTURE_TYPE_B: return PictureType::B;
            default: return PictureType::Unknown;
            }
        }

        /// A growing byte buffer behind a writable, seekable AVIOContext.
        struct MemoryOutput
        {
            std::vector<std::uint8_t> bytes;
            std::size_t position = 0;

            static int write(void* opaque, const std::uint8_t* data, const int size)
            {
                auto& output = *static_cast<MemoryOutput*>(opaque);
                const std::size_t end = output.position + static_cast<std::size_t>(size);
                if (end > output.bytes.size())
                {
                    output.bytes.resize(end);
                }
                std::memcpy(output.bytes.data() + output.position, data, static_cast<std::size_t>(size));
                output.position = end;
                return size;
            }

            static std::int64_t seek(void* opaque, const std::int64_t offset, const int whence)
            {
                auto& output = *static_cast<MemoryOutput*>(opaque);
                const auto size = static_cast<std::int64_t>(output.bytes.size());
                if ((whence & AVSEEK_SIZE) != 0)
                {
                    return size;
                }
                std::int64_t target = 0;
                switch (whence & ~AVSEEK_FORCE)
                {
                case SEEK_SET: target = offset; break;
                case SEEK_CUR: target = static_cast<std::int64_t>(output.position) + offset; break;
                case SEEK_END: target = size + offset; break;
                default: return AVERROR(EINVAL);
                }
                if (target < 0)
                {
                    return AVERROR(EINVAL);
                }
                output.position = static_cast<std::size_t>(target);
                return target;
            }
        };

        /// Detaches the custom I/O context before the format context is freed,
        /// since freeing it would otherwise try to close that context as a
        /// file.
        struct DetachCustomIo
        {
            AVFormatContext* format;
            ~DetachCustomIo() { format->pb = nullptr; }
        };
    }

    EncoderSession::EncoderSession(const EncoderSetup& setup)
    {
        const AVCodec* codec = avcodec_find_encoder_by_name(setup.encoder_name.c_str());
        if (codec == nullptr)
        {
            throw UnsupportedCapability("encoder", setup.encoder_name, build_info().identity_hash);
        }

        m_context.reset(LL_FF_ALLOC(avcodec_alloc_context3(codec)));
        m_log_capture.emplace(m_context.get(),
                              [](const int level, const char*, va_list) { return level >= AV_LOG_INFO; });

        AVCodecContext& context = *m_context;
        context.width = setup.width;
        context.height = setup.height;
        context.pix_fmt = static_cast<AVPixelFormat>(setup.pixel_format.raw());
        context.framerate = AVRational{setup.frame_rate.num, setup.frame_rate.den};
        context.time_base = AVRational{setup.frame_rate.den, setup.frame_rate.num};
        context.sample_aspect_ratio = AVRational{setup.sample_aspect_ratio.num, setup.sample_aspect_ratio.den};
        if (setup.icc_profile != nullptr)
        {
            const std::vector<std::uint8_t>& bytes = setup.icc_profile->bytes;
            const AVFrameSideData* profile =
                LL_FF_ALLOC(av_frame_side_data_new(&context.decoded_side_data, &context.nb_decoded_side_data,
                                                   AV_FRAME_DATA_ICC_PROFILE, bytes.size(), 0));
            std::memcpy(profile->data, bytes.data(), bytes.size());
        }
        if (setup.orientation.has_value())
        {
            const AVFrameSideData* matrix =
                LL_FF_ALLOC(av_frame_side_data_new(&context.decoded_side_data, &context.nb_decoded_side_data,
                                                   AV_FRAME_DATA_DISPLAYMATRIX, sizeof(std::int32_t) * 9, 0));
            LL_FF_CHECK(av_exif_orientation_to_matrix(reinterpret_cast<std::int32_t*>(matrix->data),
                                                      *setup.orientation));

            // Some encoders (PNG's) write the orientation from an EXIF block
            // rather than from the matrix; this one holds nothing else.
            AVExifMetadata exif{};
            const std::uint64_t orientation = static_cast<std::uint64_t>(*setup.orientation);
            constexpr std::uint16_t orientation_tag = 0x0112;
            AVBufferRef* raw_block = nullptr;
            const int set = av_exif_set_entry(m_context.get(), &exif, orientation_tag, AV_TIFF_SHORT, 1, nullptr, 0,
                                              &orientation);
            const int written = set < 0 ? set : av_exif_write(m_context.get(), &exif, &raw_block, AV_EXIF_TIFF_HEADER);
            av_exif_free(&exif);
            LL_FF_CHECK(written);
            const BufferRefPtr block(raw_block);
            AVFrameSideData* exif_side_data =
                LL_FF_ALLOC(av_frame_side_data_new(&context.decoded_side_data, &context.nb_decoded_side_data,
                                                   AV_FRAME_DATA_EXIF, block->size, 0));
            std::memcpy(exif_side_data->data, block->data, block->size);
        }
        context.colorspace = static_cast<AVColorSpace>(setup.color.matrix);
        context.color_range = static_cast<AVColorRange>(setup.color.range);
        context.color_primaries = static_cast<AVColorPrimaries>(setup.color.primaries);
        context.color_trc = static_cast<AVColorTransferCharacteristic>(setup.color.transfer);
        context.chroma_sample_location = static_cast<AVChromaLocation>(setup.color.chroma_location);
        context.thread_count = setup.thread_count;

        // Keeps FFmpeg's version string out of the bitstream, so the bytes
        // depend on the input and the settings alone.
        context.flags |= AV_CODEC_FLAG_BITEXACT;

        if (setup.fixed_qscale.has_value())
        {
            context.flags |= AV_CODEC_FLAG_QSCALE;
            context.global_quality = *setup.fixed_qscale * FF_QP2LAMBDA;
        }
        if (setup.qmin.has_value())
        {
            context.qmin = *setup.qmin;
        }
        if (setup.qmax.has_value())
        {
            context.qmax = *setup.qmax;
        }
        context.bit_rate = setup.bit_rate;
        context.rc_max_rate = setup.max_rate;
        if (setup.buffer_size > std::numeric_limits<int>::max())
        {
            throw ConfigError("buffer size " + std::to_string(setup.buffer_size) + " is out of range");
        }
        context.rc_buffer_size = static_cast<int>(setup.buffer_size);
        if (setup.gop_size.has_value())
        {
            context.gop_size = *setup.gop_size;
        }
        if (setup.keyint_min.has_value())
        {
            context.keyint_min = *setup.keyint_min;
        }
        if (setup.max_b_frames.has_value())
        {
            context.max_b_frames = *setup.max_b_frames;
        }
        if (setup.closed_gop)
        {
            context.flags = static_cast<int>(static_cast<unsigned int>(context.flags) | AV_CODEC_FLAG_CLOSED_GOP);
        }
        if (setup.global_header)
        {
            context.flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        }
        m_global_header = setup.global_header;

        AVDictionary* raw_options = nullptr;
        for (const auto& [name, value] : setup.options)
        {
            LL_FF_CHECK(av_dict_set(&raw_options, name.c_str(), value.c_str(), 0));
            m_option_names.push_back(name);
        }
        const int opened = LL_FF_TIMED(avcodec_open2(&context, codec, &raw_options));
        const DictionaryPtr unused_options(raw_options);
        LL_FF_CHECK(opened);
        if (const AVDictionaryEntry* unused = av_dict_iterate(unused_options.get(), nullptr))
        {
            throw ConfigError("encoder '" + setup.encoder_name + "' did not accept option '" + unused->key + "'");
        }
    }

    void EncoderSession::send(const Frame& frame, const std::int64_t pts)
    {
        const FramePtr input = ref_frame(frame.raw());
        input->pts = pts;
        input->pict_type = AV_PICTURE_TYPE_NONE;

        // A fixed-quantizer encoder reads the quantizer from each frame.
        input->quality = (m_context->flags & AV_CODEC_FLAG_QSCALE) != 0 ? m_context->global_quality : 0;
        input->colorspace = m_context->colorspace;
        input->color_range = m_context->color_range;
        input->color_primaries = m_context->color_primaries;
        input->color_trc = m_context->color_trc;
        input->chroma_location = m_context->chroma_sample_location;
        input->sample_aspect_ratio = m_context->sample_aspect_ratio;

        // The frame states the ICC profile and orientation the encoder was
        // opened with, and nothing else of that kind.
        av_frame_remove_side_data(input.get(), AV_FRAME_DATA_ICC_PROFILE);
        av_frame_remove_side_data(input.get(), AV_FRAME_DATA_DISPLAYMATRIX);
        av_frame_remove_side_data(input.get(), AV_FRAME_DATA_EXIF);
        for (int i = 0; i < m_context->nb_decoded_side_data; ++i)
        {
            LL_FF_CHECK(av_frame_side_data_clone(&input->side_data, &input->nb_side_data,
                                                 m_context->decoded_side_data[i], 0));
        }

        while (true)
        {
            const int sent = LL_FF_TIMED(avcodec_send_frame(m_context.get(), input.get()));
            if (sent == AVERROR(EAGAIN))
            {
                drain();
                continue;
            }
            LL_FF_CHECK(sent);
            break;
        }
        drain();
    }

    void EncoderSession::finish()
    {
        LL_FF_CHECK(avcodec_send_frame(m_context.get(), nullptr));
        drain();
    }

    void EncoderSession::drain()
    {
        while (true)
        {
            PacketPtr packet = make_packet();
            const int received = LL_FF_TIMED(avcodec_receive_packet(m_context.get(), packet.get()));
            if (received == AVERROR(EAGAIN) || received == AVERROR_EOF)
            {
                return;
            }
            LL_FF_CHECK(received);
            m_packets.push_back(std::move(packet));
        }
    }

    EncoderResolution EncoderSession::resolved_settings() const
    {
        const AVCodecContext& context = *m_context;
        EncoderResolution resolution;
        for (const std::string& name : m_option_names)
        {
            std::uint8_t* value = nullptr;
            if (av_opt_get(context.priv_data, name.c_str(), 0, &value) >= 0 && value != nullptr)
            {
                resolution.options[name] = std::string(reinterpret_cast<const char*>(value));
            }
            av_free(value);
        }

        resolution.time_base = Rational{context.time_base.num, context.time_base.den};
        resolution.thread_count = context.thread_count;
        resolution.bitexact = (context.flags & AV_CODEC_FLAG_BITEXACT) != 0;
        if ((context.flags & AV_CODEC_FLAG_QSCALE) != 0)
        {
            resolution.fixed_qscale = context.global_quality / FF_QP2LAMBDA;
        }
        resolution.qmin = context.qmin;
        resolution.qmax = context.qmax;
        resolution.bit_rate = context.bit_rate;
        resolution.max_rate = context.rc_max_rate;
        resolution.buffer_size = context.rc_buffer_size;
        resolution.gop_size = context.gop_size;
        resolution.keyint_min = context.keyint_min;
        resolution.max_b_frames = context.max_b_frames;
        resolution.closed_gop = (static_cast<unsigned int>(context.flags) & AV_CODEC_FLAG_CLOSED_GOP) != 0;
        resolution.global_header = m_global_header;
        return resolution;
    }

    std::vector<std::uint8_t> concatenate_packets(const std::vector<PacketPtr>& packets)
    {
        std::vector<std::uint8_t> bytes;
        for (const PacketPtr& packet : packets)
        {
            bytes.insert(bytes.end(), packet->data, packet->data + packet->size);
        }
        return bytes;
    }

    bool muxer_wants_global_header(const std::string& muxer_name)
    {
        const AVOutputFormat* muxer = av_guess_format(muxer_name.c_str(), nullptr, nullptr);
        return muxer != nullptr && (muxer->flags & AVFMT_GLOBALHEADER) != 0;
    }

    std::vector<std::uint8_t> mux_packets(const std::string& muxer_name, const AVCodecContext& encoder,
                                          std::vector<PacketPtr>& packets)
    {
        const AVOutputFormat* muxer = av_guess_format(muxer_name.c_str(), nullptr, nullptr);
        if (muxer == nullptr)
        {
            throw UnsupportedCapability("muxer", muxer_name, build_info().identity_hash);
        }

        AVFormatContext* raw_format = nullptr;
        LL_FF_CHECK(avformat_alloc_output_context2(&raw_format, muxer, nullptr, nullptr));
        const OutputFormatContextPtr format(raw_format);

        MemoryOutput output;
        constexpr int buffer_size = 64 * 1024;
        auto* buffer = static_cast<unsigned char*>(LL_FF_ALLOC(av_malloc(buffer_size)));
        AVIOContext* raw_io =
            avio_alloc_context(buffer, buffer_size, 1, &output, nullptr, &MemoryOutput::write, &MemoryOutput::seek);
        if (raw_io == nullptr)
        {
            av_free(buffer);
            throw_ff_error(AVERROR(ENOMEM), "avio_alloc_context");
        }
        const AvIoContextPtr io(raw_io);
        format->pb = io.get();
        format->flags |= AVFMT_FLAG_CUSTOM_IO | AVFMT_FLAG_BITEXACT;
        const DetachCustomIo detach{format.get()};

        AVStream* stream = LL_FF_ALLOC(avformat_new_stream(format.get(), nullptr));
        LL_FF_CHECK(avcodec_parameters_from_context(stream->codecpar, &encoder));
        stream->time_base = encoder.time_base;
        stream->avg_frame_rate = encoder.framerate;
        stream->sample_aspect_ratio = encoder.sample_aspect_ratio;
        for (int i = 0; i < encoder.nb_decoded_side_data; ++i)
        {
            const AVFrameSideData& side_data = *encoder.decoded_side_data[i];
            AVPacketSideDataType type = AV_PKT_DATA_NB;
            if (side_data.type == AV_FRAME_DATA_ICC_PROFILE)
            {
                type = AV_PKT_DATA_ICC_PROFILE;
            }
            else if (side_data.type == AV_FRAME_DATA_DISPLAYMATRIX)
            {
                type = AV_PKT_DATA_DISPLAYMATRIX;
            }
            if (type == AV_PKT_DATA_NB ||
                av_packet_side_data_get(stream->codecpar->coded_side_data, stream->codecpar->nb_coded_side_data,
                                        type) != nullptr)
            {
                continue;
            }
            AVPacketSideData* copy = LL_FF_ALLOC(av_packet_side_data_new(
                &stream->codecpar->coded_side_data, &stream->codecpar->nb_coded_side_data, type, side_data.size, 0));
            std::memcpy(copy->data, side_data.data, side_data.size);
        }

        LL_FF_CHECK(avformat_write_header(format.get(), nullptr));
        for (PacketPtr& packet : packets)
        {
            packet->stream_index = stream->index;
            av_packet_rescale_ts(packet.get(), encoder.time_base, stream->time_base);
            LL_FF_CHECK(av_interleaved_write_frame(format.get(), packet.get()));
        }
        LL_FF_CHECK(av_write_trailer(format.get()));
        avio_flush(io.get());
        return std::move(output.bytes);
    }

    FrameStats packet_stats(const AVPacket& packet, const bool reports_qp)
    {
        FrameStats stats;
        stats.pts = packet.pts;
        stats.key_frame = (packet.flags & AV_PKT_FLAG_KEY) != 0;
        stats.size_bytes = packet.size;
        stats.picture_type = stats.key_frame ? PictureType::I : PictureType::Unknown;

        std::size_t size = 0;
        const std::uint8_t* quality = av_packet_get_side_data(&packet, AV_PKT_DATA_QUALITY_STATS, &size);
        if (quality != nullptr && size >= 5)
        {
            if (const PictureType type = picture_type_of(quality[4]); type != PictureType::Unknown)
            {
                stats.picture_type = type;
            }
            if (reports_qp)
            {
                const double qp = static_cast<double>(AV_RL32(quality)) / FF_QP2LAMBDA;
                stats.qp_min = qp;
                stats.qp_max = qp;
                stats.qp_mean = qp;
            }
        }
        return stats;
    }
}
