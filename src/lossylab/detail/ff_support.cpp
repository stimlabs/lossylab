#include "lossylab/detail/ff_error.hpp"
#include "lossylab/detail/ff_ptr.hpp"

#include <array>
#include <cstdio>

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
