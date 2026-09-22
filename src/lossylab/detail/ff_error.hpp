#pragma once

/// Translation of FFmpeg's integer error returns into the library's exception
/// hierarchy. Internal header.

#include "lossylab/core/error.hpp"

extern "C" {
#include <libavutil/error.h>
}

#include <cerrno>
#include <string>

namespace lossylab::detail
{
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
/// any exception it raises.
#define LL_FF_CHECK(expr) ::lossylab::detail::check((expr), #expr)

/// Wraps an FFmpeg call that returns a pointer, throwing when it returns null.
#define LL_FF_ALLOC(expr) ::lossylab::detail::check_alloc((expr), #expr)
