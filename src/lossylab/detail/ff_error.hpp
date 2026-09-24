#pragma once

/// Translation of FFmpeg's integer error returns into the library's exception
/// hierarchy. Internal header.

#include "lossylab/core/error.hpp"

extern "C" {
#include <libavutil/error.h>
}

#include <cerrno>
#include <chrono>
#include <string>
#include <utility>

namespace lossylab::detail
{
    /// Milliseconds the calling thread has spent inside timed FFmpeg calls so
    /// far. A stage reads it before and after its work to learn how much of
    /// its wall-clock time FFmpeg accounts for.
    [[nodiscard]] double ffmpeg_elapsed_ms() noexcept;

    /// Adds `milliseconds` to the calling thread's FFmpeg total.
    void add_ffmpeg_elapsed_ms(double milliseconds) noexcept;

    /// Runs `call` and adds its wall-clock time to the calling thread's FFmpeg
    /// total, also when it throws. Calls must not nest, or their time counts
    /// twice.
    template <typename Call>
    decltype(auto) time_ffmpeg(Call&& call)
    {
        struct Accumulator
        {
            std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();

            ~Accumulator()
            {
                add_ffmpeg_elapsed_ms(
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count());
            }
        } const accumulator;
        return std::forward<Call>(call)();
    }

    /// Wall-clock time of one stage, and the part of it spent inside FFmpeg.
    class StageClock
    {
    public:
        /// Milliseconds since construction.
        [[nodiscard]] double duration_ms() const noexcept
        {
            return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - m_started).count();
        }

        /// Milliseconds of that time spent inside timed FFmpeg calls on this thread.
        [[nodiscard]] double ffmpeg_duration_ms() const noexcept { return ffmpeg_elapsed_ms() - m_ffmpeg_baseline; }

    private:
        std::chrono::steady_clock::time_point m_started = std::chrono::steady_clock::now();
        double m_ffmpeg_baseline = ffmpeg_elapsed_ms();
    };

    /// `av_strerror`'s text for a code, or a hex fallback when FFmpeg has no
    /// description for it.
    [[nodiscard]] std::string averror_string(int averror);

    /// Throws FFmpegError describing `averror`, attributed to `call`.
    [[noreturn]] void throw_ff_error(int averror, const char* call);

    /// Returns `result` when it is non-negative, and throws otherwise.
    /// Used for the FFmpeg calls that return a count or a size.
    inline int check(const int result, const char* call)
    {
        if (result < 0)
        {
            throw_ff_error(result, call);
        }
        return result;
    }

    /// Throws when `pointer` is null, reporting an allocation failure.
    template <typename T>
    T* check_alloc(T* pointer, const char* call)
    {
        if (pointer == nullptr)
        {
            throw_ff_error(AVERROR(ENOMEM), call);
        }
        return pointer;
    }
}

/// Wraps an FFmpeg call that returns a negative error code, naming the call in
/// any exception it raises, and counts its time as FFmpeg time.
#define LL_FF_CHECK(expr) ::lossylab::detail::check(::lossylab::detail::time_ffmpeg([&] { return (expr); }), #expr)

/// Counts the time of an FFmpeg call as FFmpeg time and returns its result,
/// for the calls whose status the caller inspects itself.
#define LL_FF_TIMED(expr) ::lossylab::detail::time_ffmpeg([&] { return (expr); })

/// Wraps an FFmpeg call that returns a pointer, throwing when it returns null.
#define LL_FF_ALLOC(expr) ::lossylab::detail::check_alloc((expr), #expr)
