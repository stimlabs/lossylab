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
}
