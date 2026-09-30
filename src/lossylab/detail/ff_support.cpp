#include "lossylab/detail/ff_error.hpp"
#include "lossylab/detail/ff_ptr.hpp"

extern "C" {
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
}

#include <array>
#include <cstdio>
#include <string>

namespace lossylab::detail
{
    std::string averror_string(const int averror)
    {
        std::array<char, AV_ERROR_MAX_STRING_SIZE> buffer{};
        if (av_strerror(averror, buffer.data(), buffer.size()) < 0)
        {
            // FFmpeg has no description for this code; report the raw value
            // rather than an empty string.
            std::array<char, 32> fallback{};
            std::snprintf(fallback.data(), fallback.size(), "error %d (0x%08x)", averror,
                          static_cast<unsigned>(averror));
            return fallback.data();
        }
        return buffer.data();
    }

    namespace
    {
        thread_local double t_ffmpeg_elapsed_ms = 0.0;
    }

    double ffmpeg_elapsed_ms() noexcept
    {
        return t_ffmpeg_elapsed_ms;
    }

    void add_ffmpeg_elapsed_ms(const double milliseconds) noexcept
    {
        t_ffmpeg_elapsed_ms += milliseconds;
    }

    void throw_ff_error(const int averror, const char* call)
    {
        throw FFmpegError(averror, call, averror_string(averror));
    }

    FramePtr make_frame()
    {
        return FramePtr(LL_FF_ALLOC(av_frame_alloc()));
    }

    PacketPtr make_packet()
    {
        return PacketPtr(LL_FF_ALLOC(av_packet_alloc()));
    }

    FilterGraphPtr make_filter_graph()
    {
        return FilterGraphPtr(LL_FF_ALLOC(avfilter_graph_alloc()));
    }

    FramePtr clone_frame(const AVFrame* source)
    {
        return FramePtr(LL_FF_ALLOC(av_frame_clone(source)));
    }

    FramePtr ref_frame(const AVFrame* source)
    {
        FramePtr copy = make_frame();
        LL_FF_CHECK(av_frame_ref(copy.get(), source));
        return copy;
    }

    namespace
    {
        /// The start of every plane of `frame` at pixel (x, y), after
        /// checking that it falls on a whole chroma sample and a whole byte.
        std::array<std::uint8_t*, 4> plane_corners(const AVFrame& frame, const int x, const int y)
        {
            const auto format = static_cast<AVPixelFormat>(frame.format);
            const AVPixFmtDescriptor* descriptor = av_pix_fmt_desc_get(format);
            const bool bit_packed = (descriptor->flags & AV_PIX_FMT_FLAG_BITSTREAM) != 0;
            if (x < 0 || y < 0 || x % (1 << descriptor->log2_chroma_w) != 0 ||
                y % (1 << descriptor->log2_chroma_h) != 0 ||
                (bit_packed && x * av_get_bits_per_pixel(descriptor) % 8 != 0))
            {
                throw ConfigError("(" + std::to_string(x) + ", " + std::to_string(y) +
                                  ") is not a whole chroma sample and byte of " + descriptor->name);
            }

            std::array<std::uint8_t*, 4> corners = {frame.data[0], frame.data[1], frame.data[2], frame.data[3]};
            for (int plane_index = 0; plane_index < av_pix_fmt_count_planes(format); ++plane_index)
            {
                const auto plane = static_cast<std::size_t>(plane_index);
                const bool is_chroma = plane_index == 1 || plane_index == 2;
                const int row = is_chroma ? y >> descriptor->log2_chroma_h : y;
                const int column_bytes = LL_FF_CHECK(av_image_get_linesize(format, x, plane_index));
                corners[plane] += static_cast<std::ptrdiff_t>(row) * frame.linesize[plane] + column_bytes;
            }
            return corners;
        }
    }

    void copy_rectangle(const AVFrame& source, const int source_x, const int source_y, AVFrame& target,
                        const int target_x, const int target_y, const int width, const int height)
    {
        if (source.format != target.format)
        {
            throw ConfigError("copy_rectangle() between two pixel formats");
        }
        if (source_x + width > source.width || source_y + height > source.height ||
            target_x + width > target.width || target_y + height > target.height)
        {
            throw ConfigError("copy_rectangle() reaches outside a frame");
        }
        const std::array<std::uint8_t*, 4> source_corners = plane_corners(source, source_x, source_y);
        const std::array<std::uint8_t*, 4> target_corners = plane_corners(target, target_x, target_y);
        const std::array<const std::uint8_t*, 4> source_data = {source_corners[0], source_corners[1],
                                                                source_corners[2], source_corners[3]};
        av_image_copy(target_corners.data(), target.linesize, source_data.data(), source.linesize,
                      static_cast<AVPixelFormat>(source.format), width, height);
    }

    Sha256Stream::Sha256Stream()
    {
        AVHashContext* context = nullptr;
        LL_FF_CHECK(av_hash_alloc(&context, "SHA256"));
        m_context.reset(context);
        av_hash_init(m_context.get());
    }

    void Sha256Stream::feed(const std::span<const std::uint8_t> chunk)
    {
        av_hash_update(m_context.get(), chunk.data(), chunk.size());
    }

    std::string Sha256Stream::finish() const
    {
        std::array<char, 2 * AV_HASH_MAX_SIZE + 1> hex{};
        av_hash_final_hex(m_context.get(), reinterpret_cast<std::uint8_t*>(hex.data()),
                          static_cast<int>(hex.size()));
        return std::string("sha256:") + hex.data();
    }

    std::string sha256_hex(const std::span<const std::uint8_t> bytes)
    {
        Sha256Stream stream;
        stream.feed(bytes);
        return stream.finish();
    }
}
