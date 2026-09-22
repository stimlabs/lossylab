#pragma once

/// Claiming FFmpeg log calls made against one context. Internal header.

#include <cstdarg>
#include <functional>

namespace lossylab::detail
{
    /// Receives an FFmpeg log call before it is formatted. Returns true when
    /// it consumed the call, which then goes no further. It must read `args`
    /// only when it returns true, and must not throw: it runs inside FFmpeg.
    using LogSink = std::function<bool(int level, const char* format, va_list args)>;

    /// While alive, hands every log call the current thread makes against
    /// `context` to `sink` before the library's log handler sees it. Installs
    /// the library's log callback if it is not installed yet.
    class ScopedLogCapture
    {
    public:
        ScopedLogCapture(const void* context, LogSink sink);
        ~ScopedLogCapture();

        ScopedLogCapture(const ScopedLogCapture&) = delete;
        ScopedLogCapture& operator=(const ScopedLogCapture&) = delete;
        ScopedLogCapture(ScopedLogCapture&&) = delete;
        ScopedLogCapture& operator=(ScopedLogCapture&&) = delete;

    private:
        const void* m_context;
        LogSink m_sink;
        ScopedLogCapture* m_previous;

        friend bool dispatch_to_capture(const void* context, int level, const char* format, va_list args);
    };

    /// Offers a log call to the capture active on this thread. Returns true
    /// when a capture consumed it.
    bool dispatch_to_capture(const void* context, int level, const char* format, va_list args);
}
