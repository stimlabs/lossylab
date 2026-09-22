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
    /// `context` to `sink`, ahead of any LogCapture and the log handler.
    /// Installs the library's log callback.
    class ContextLogCapture
    {
    public:
        ContextLogCapture(const void* context, LogSink sink);
        ~ContextLogCapture();

        ContextLogCapture(const ContextLogCapture&) = delete;
        ContextLogCapture& operator=(const ContextLogCapture&) = delete;
        ContextLogCapture(ContextLogCapture&&) = delete;
        ContextLogCapture& operator=(ContextLogCapture&&) = delete;

    private:
        const void* m_context;
        LogSink m_sink;
        ContextLogCapture* m_previous;

        friend bool dispatch_to_context_capture(const void* context, int level, const char* format, va_list args);
    };

    /// Offers a log call to the context captures active on this thread.
    /// Returns true when one consumed it.
    bool dispatch_to_context_capture(const void* context, int level, const char* format, va_list args);
}
