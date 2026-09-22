#include "lossylab/env/log.hpp"

#include "lossylab/core/error.hpp"

#include <array>
#include <mutex>
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

        void log_callback(void* avcl, const int level, const char* fmt, va_list args)
        {
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
                return;
            }

            if (level > av_log_get_level())
            {
                return;
            }

            // FFmpeg reassembles partial lines across calls, so print_prefix
            // has to be carried rather than passed as a literal.
            int print_prefix = 1;
            std::array<char, 1024> buffer{};
            const int written = av_log_format_line2(avcl, level, fmt, args, buffer.data(),
                                                    static_cast<int>(buffer.size()),
                                                    &print_prefix);
            if (written <= 0)
            {
                return;
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

            handler(message);
        }
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

        // Restoring FFmpeg's default when the handler is cleared keeps this
        // from silently swallowing diagnostics.
        const std::lock_guard<std::mutex> lock(state.mutex);
        av_log_set_callback(state.handler ? log_callback : av_log_default_callback);
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
