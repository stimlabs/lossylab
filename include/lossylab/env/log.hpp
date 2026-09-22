#pragma once

#include "lossylab/core/json.hpp"

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace lossylab
{
    namespace detail
    {
        struct LogCaptureAccess;
    }

    /// Mirrors FFmpeg's log levels, in the same order.
    enum class LogLevel
    {
        Quiet,
        Panic,
        Fatal,
        Error,
        Warning,
        Info,
        Verbose,
        Debug,
        Trace
    };

    std::string to_string(LogLevel level);
    LogLevel log_level_from_string(std::string_view name);

    /// One log line from FFmpeg.
    struct LogMessage
    {
        LogLevel level = LogLevel::Info;

        /// The component that emitted it, e.g. "libx264" or "swscaler", taken
        /// from the AVClass of the object being logged. Empty when FFmpeg
        /// logged without a context.
        std::string component;

        /// The message, with the trailing newline removed.
        std::string text;

        [[nodiscard]] json::Value to_json() const;
        static LogMessage from_json(const json::Value& value);
    };

    bool operator==(const LogMessage& left, const LogMessage& right) noexcept;
    inline bool operator!=(const LogMessage& left, const LogMessage& right) noexcept
    {
        return !(left == right);
    }

    /// Receives FFmpeg log lines.
    ///
    /// Called on whatever thread produced the message, which for a threaded
    /// encoder is not the caller's, so an implementation must be thread-safe.
    using LogHandler = std::function<void(const LogMessage&)>;

    /// Routes FFmpeg's log output to `handler`, at or above `level`.
    ///
    /// Worth doing even when logs seem uninteresting: FFmpeg reports several
    /// things only here, including format negotiation decisions and clipping
    /// during color conversion. Those are precisely the silent changes this
    /// library exists to make visible.
    ///
    /// Passing an empty handler restores FFmpeg's default output, which
    /// writes to stderr. This installs a process-global callback, so it
    /// affects every user of FFmpeg in the process; call it once, from the
    /// main thread, before any work starts. `read_headers()` and LogCapture
    /// install the same callback; lines they capture never reach the handler.
    void set_log_handler(LogHandler handler, LogLevel level = LogLevel::Info);

    /// Collects the FFmpeg log lines the current thread produces while it is
    /// alive, so they can be attributed to the file that caused them rather
    /// than interleaved with every other thread's in the log handler.
    ///
    /// Collects lines at or above `level` whatever level `set_log_handler` or
    /// `mute_log` set, and those lines do not reach the handler. Lines below
    /// `level` go to the handler as usual. Captures nest: only the innermost
    /// one on a thread collects.
    ///
    /// Lines FFmpeg logs from its own worker threads, which frame-threaded
    /// decoding with `thread_count` above 1 uses, are not attributed and go to
    /// the handler. Must be destroyed on the thread that created it, which a
    /// local variable always is.
    class LogCapture
    {
    public:
        explicit LogCapture(LogLevel level = LogLevel::Warning);
        ~LogCapture();

        LogCapture(const LogCapture&) = delete;
        LogCapture& operator=(const LogCapture&) = delete;
        LogCapture(LogCapture&&) = delete;
        LogCapture& operator=(LogCapture&&) = delete;

        [[nodiscard]] LogLevel level() const noexcept { return m_level; }

        [[nodiscard]] const std::vector<LogMessage>& messages() const noexcept { return m_messages; }

        /// The lines collected so far, leaving the capture empty.
        [[nodiscard]] std::vector<LogMessage> take();

    private:
        friend struct detail::LogCaptureAccess;

        LogLevel m_level;
        std::vector<LogMessage> m_messages;
        LogCapture* m_previous;
    };

    /// Silences FFmpeg entirely. Equivalent to setting the level to Quiet.
    void mute_log();

    /// The level currently in effect.
    [[nodiscard]] LogLevel log_level();
}
