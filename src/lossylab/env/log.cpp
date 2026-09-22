#include "lossylab/env/log.hpp"

#include "lossylab/core/error.hpp"
#include "lossylab/detail/log_capture.hpp"

#include <array>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

extern "C" {
#include <libavutil/log.h>
}

namespace lossylab
{
    namespace
    {
        int to_av_level(const LogLevel level)
        {
            switch (level)
            {
            case LogLevel::Quiet: return AV_LOG_QUIET;
            case LogLevel::Panic: return AV_LOG_PANIC;
            case LogLevel::Fatal: return AV_LOG_FATAL;
            case LogLevel::Error: return AV_LOG_ERROR;
            case LogLevel::Warning: return AV_LOG_WARNING;
            case LogLevel::Info: return AV_LOG_INFO;
            case LogLevel::Verbose: return AV_LOG_VERBOSE;
            case LogLevel::Debug: return AV_LOG_DEBUG;
            case LogLevel::Trace: return AV_LOG_TRACE;
            }
            return AV_LOG_INFO;
        }

        LogLevel from_av_level(const int level)
        {
            if (level <= AV_LOG_QUIET) { return LogLevel::Quiet; }
            if (level <= AV_LOG_PANIC) { return LogLevel::Panic; }
            if (level <= AV_LOG_FATAL) { return LogLevel::Fatal; }
            if (level <= AV_LOG_ERROR) { return LogLevel::Error; }
            if (level <= AV_LOG_WARNING) { return LogLevel::Warning; }
            if (level <= AV_LOG_INFO) { return LogLevel::Info; }
            if (level <= AV_LOG_VERBOSE) { return LogLevel::Verbose; }
            if (level <= AV_LOG_DEBUG) { return LogLevel::Debug; }
            return LogLevel::Trace;
        }

        /// The installed handler, behind a mutex.
        ///
        /// FFmpeg calls the log callback from encoder worker threads, so both
        /// the handler itself and the act of replacing it have to be guarded.
        struct HandlerState
        {
            std::mutex mutex;
            LogHandler handler;
        };

        HandlerState& handler_state()
        {
            // Function-local static, so nothing is constructed at load time.
            static HandlerState state;
            return state;
        }

        /// The context capture most recently opened on this thread; each one
        /// links to the one it shadows.
        thread_local detail::ContextLogCapture* active_context_capture = nullptr;

        /// Likewise for LogCapture.
        thread_local LogCapture* active_log_capture = nullptr;

        std::optional<LogMessage> format_message(void* avcl, const int level, const char* fmt, va_list args)
        {
            // FFmpeg reassembles partial lines across calls, so print_prefix
            // has to be carried rather than passed as a literal.
            int print_prefix = 1;
            std::array<char, 1024> buffer{};
            const int written = av_log_format_line2(avcl, level, fmt, args, buffer.data(),
                                                    static_cast<int>(buffer.size()),
                                                    &print_prefix);
            if (written <= 0)
            {
                return std::nullopt;
            }

            LogMessage message;
            message.level = from_av_level(level);
            message.text.assign(buffer.data(),
                                static_cast<std::size_t>(
                                    std::min(written, static_cast<int>(buffer.size()) - 1)));
            while (!message.text.empty() &&
                   (message.text.back() == '\n' || message.text.back() == '\r'))
            {
                message.text.pop_back();
            }

            if (avcl != nullptr)
            {
                if (const AVClass* klass = *static_cast<AVClass**>(avcl);
                    klass != nullptr && klass->item_name != nullptr)
                {
                    const char* name = klass->item_name(avcl);
                    message.component = name != nullptr ? name : "";
                }
            }
            return message;
        }
    }

    namespace detail
    {
        /// LogCapture's side of the log callback.
        struct LogCaptureAccess
        {
            /// Appends the call to this thread's innermost LogCapture when
            /// its level admits it. Returns true when it did.
            static bool collect(void* avcl, const int level, const char* format, va_list args) noexcept
            {
                LogCapture* capture = active_log_capture;
                if (capture == nullptr || from_av_level(level) > capture->m_level)
                {
                    return false;
                }
                try
                {
                    if (std::optional<LogMessage> message = format_message(avcl, level, format, args))
                    {
                        capture->m_messages.push_back(std::move(*message));
                    }
                }
                catch (...)
                {
                    // Out of memory inside FFmpeg's callback: the line is
                    // dropped rather than thrown through C code.
                }
                return true;
            }

            static LogCapture*& active() noexcept { return active_log_capture; }
        };
    }

    namespace
    {
        void log_callback(void* avcl, const int level, const char* fmt, va_list args)
        {
            if (detail::dispatch_to_context_capture(avcl, level, fmt, args))
            {
                return;
            }
            if (detail::LogCaptureAccess::collect(avcl, level, fmt, args))
            {
                return;
            }

            HandlerState& state = handler_state();

            // Copied under the lock and invoked outside it: a handler that
            // calls back into FFmpeg would otherwise deadlock.
            LogHandler handler;
            {
                const std::lock_guard<std::mutex> lock(state.mutex);
                handler = state.handler;
            }
            if (!handler)
            {
                av_log_default_callback(avcl, level, fmt, args);
                return;
            }

            if (level > av_log_get_level())
            {
                return;
            }

            if (const std::optional<LogMessage> message = format_message(avcl, level, fmt, args))
            {
                handler(*message);
            }
        }

        /// Reinstalled on every call rather than once, in case another
        /// component in the process replaced it since.
        void install_log_callback()
        {
            av_log_set_callback(log_callback);
        }
    }

    namespace detail
    {
        ContextLogCapture::ContextLogCapture(const void* context, LogSink sink)
            : m_context(context), m_sink(std::move(sink)), m_previous(active_context_capture)
        {
            install_log_callback();
            active_context_capture = this;
        }

        ContextLogCapture::~ContextLogCapture()
        {
            active_context_capture = m_previous;
        }

        bool dispatch_to_context_capture(const void* context, const int level, const char* format, va_list args)
        {
            for (const ContextLogCapture* capture = active_context_capture; capture != nullptr;
                 capture = capture->m_previous)
            {
                if (capture->m_context == context)
                {
                    return capture->m_sink(level, format, args);
                }
            }
            return false;
        }
    }

    json::Value LogMessage::to_json() const
    {
        return json::object({{"level", to_string(level)}, {"component", component}, {"text", text}});
    }

    LogMessage LogMessage::from_json(const json::Value& value)
    {
        LogMessage message;
        message.level = log_level_from_string(value.at("level").get<std::string>());
        message.component = json::string_or(value, "component", "");
        message.text = value.at("text").get<std::string>();
        return message;
    }

    bool operator==(const LogMessage& left, const LogMessage& right) noexcept
    {
        return left.level == right.level && left.component == right.component && left.text == right.text;
    }

    LogCapture::LogCapture(const LogLevel level)
        : m_level(level), m_previous(detail::LogCaptureAccess::active())
    {
        install_log_callback();
        detail::LogCaptureAccess::active() = this;
    }

    LogCapture::~LogCapture()
    {
        detail::LogCaptureAccess::active() = m_previous;
    }

    std::vector<LogMessage> LogCapture::take()
    {
        return std::exchange(m_messages, {});
    }

    std::string to_string(const LogLevel level)
    {
        switch (level)
        {
        case LogLevel::Quiet: return "quiet";
        case LogLevel::Panic: return "panic";
        case LogLevel::Fatal: return "fatal";
        case LogLevel::Error: return "error";
        case LogLevel::Warning: return "warning";
        case LogLevel::Info: return "info";
        case LogLevel::Verbose: return "verbose";
        case LogLevel::Debug: return "debug";
        case LogLevel::Trace: return "trace";
        }
        return "info";
    }

    LogLevel log_level_from_string(const std::string_view name)
    {
        if (name == "quiet") { return LogLevel::Quiet; }
        if (name == "panic") { return LogLevel::Panic; }
        if (name == "fatal") { return LogLevel::Fatal; }
        if (name == "error") { return LogLevel::Error; }
        if (name == "warning") { return LogLevel::Warning; }
        if (name == "info") { return LogLevel::Info; }
        if (name == "verbose") { return LogLevel::Verbose; }
        if (name == "debug") { return LogLevel::Debug; }
        if (name == "trace") { return LogLevel::Trace; }
        throw ConfigError("unknown log level '" + std::string(name) + "'");
    }

    void set_log_handler(LogHandler handler, const LogLevel level)
    {
        HandlerState& state = handler_state();
        {
            const std::lock_guard<std::mutex> lock(state.mutex);
            state.handler = std::move(handler);
        }

        av_log_set_level(to_av_level(level));

        // Stays installed with no handler, forwarding to FFmpeg's default,
        // so a capture on another thread keeps receiving its calls.
        install_log_callback();
    }

    void mute_log()
    {
        av_log_set_level(AV_LOG_QUIET);
    }

    LogLevel log_level()
    {
        return from_av_level(av_log_get_level());
    }
}
