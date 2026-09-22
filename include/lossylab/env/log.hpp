#pragma once

#include <functional>
#include <string>
#include <string_view>

namespace lossylab
{
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
    };

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
    /// Passing an empty handler restores FFmpeg's default, which writes to
    /// stderr. This installs a process-global callback, so it affects every
    /// user of FFmpeg in the process; call it once, from the main thread,
    /// before any work starts.
    void set_log_handler(LogHandler handler, LogLevel level = LogLevel::Info);

    /// Silences FFmpeg entirely. Equivalent to setting the level to Quiet.
    void mute_log();

    /// The level currently in effect.
    [[nodiscard]] LogLevel log_level();
}
