#pragma once

#include "lossylab/core/error.hpp"
#include "lossylab/core/json.hpp"
#include "lossylab/core/json_io.hpp"
#include "lossylab/core/reflect.hpp"
#include "lossylab/env/log.hpp"
#include "lossylab/io/source.hpp"

#include <exception>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace lossylab
{
    /// Which kind of failure a FileError describes. One member per Error
    /// subclass, plus the non-library failures an operation on an arbitrary
    /// input file can still produce.
    enum class FileErrorKind
    {
        /// ConfigError: the request was invalid for this file.
        Config,

        /// UnsupportedCapability: the file needs a codec, filter or backend
        /// this FFmpeg build lacks.
        UnsupportedCapability,

        /// ConversionRefused: strict mode refused an implicit conversion.
        ConversionRefused,

        /// FFmpegError: FFmpeg rejected the file. Corrupt, truncated and
        /// unrecognized inputs land here.
        FFmpeg,

        /// NotImplemented: the operation is declared but not written yet.
        NotImplemented,

        /// A lossylab::Error that is none of the subclasses above.
        Library,

        /// std::bad_alloc.
        OutOfMemory,

        /// Anything else, including exceptions not derived from
        /// std::exception.
        Internal
    };

    std::string to_string(FileErrorKind kind);
    FileErrorKind file_error_kind_from_string(std::string_view name);

    /// A failed operation on one input file, as data rather than as an
    /// exception, so it can be stored and aggregated alongside the results of
    /// files that succeeded.
    struct FileError
    {
        FileErrorKind kind = FileErrorKind::Internal;

        /// The entry point that failed, e.g. "probe" or "decode_image".
        std::string operation;

        /// `Source::describe()` of the input.
        std::string source;

        /// The exception's `what()`.
        std::string message;

        /// The fields the exception carries beyond its message, keyed by
        /// kind:
        ///  - UnsupportedCapability: "capability_kind", "name", "build_id"
        ///  - ConversionRefused: "from", "to", "context"
        ///  - FFmpeg: "averror", "call"
        ///  - NotImplemented: "symbol"
        /// An empty object for the other kinds.
        json::Value details = json::Value::object();

        /// Converts the exception currently being handled. Only valid inside
        /// a catch block.
        [[nodiscard]] static FileError from_current_exception(std::string operation, std::string source);

        [[nodiscard]] json::Value to_json() const;
        static FileError from_json(const json::Value& value);
    };

    LOSSYLAB_REFLECT(FileError, kind, operation, source, message, details);

    bool operator==(const FileError& left, const FileError& right) noexcept;
    inline bool operator!=(const FileError& left, const FileError& right) noexcept
    {
        return !(left == right);
    }

    /// The outcome of one operation on one input file: either its result or
    /// the FileError that replaced the exception it threw, together with the
    /// FFmpeg log lines the operation produced.
    template <typename T>
    class FileResult
    {
    public:
        explicit FileResult(T value, std::vector<LogMessage> log = {})
            : m_outcome(std::in_place_index<0>, std::move(value)), m_log(std::move(log))
        {
        }

        explicit FileResult(FileError error, std::vector<LogMessage> log = {})
            : m_outcome(std::in_place_index<1>, std::move(error)), m_log(std::move(log))
        {
        }

        [[nodiscard]] bool ok() const noexcept { return m_outcome.index() == 0; }

        /// What FFmpeg logged on the calling thread while the operation ran,
        /// whether it succeeded or not. A file that decoded
        /// but logged errors along the way is damaged, and this is where that
        /// shows.
        [[nodiscard]] const std::vector<LogMessage>& log() const noexcept { return m_log; }

        /// The result. Throws ConfigError, carrying the stored error's
        /// message, when the operation failed.
        [[nodiscard]] const T& value() const&
        {
            require_ok();
            return std::get<0>(m_outcome);
        }

        [[nodiscard]] T& value() &
        {
            require_ok();
            return std::get<0>(m_outcome);
        }

        [[nodiscard]] T&& value() &&
        {
            require_ok();
            return std::get<0>(std::move(m_outcome));
        }

        /// The error. Throws ConfigError when the operation succeeded.
        [[nodiscard]] const FileError& error() const
        {
            if (ok())
            {
                throw ConfigError("FileResult holds a value, not an error");
            }
            return std::get<1>(m_outcome);
        }

        /// {"ok": true, "value": ..., "log": [...]} or
        /// {"ok": false, "error": ..., "log": [...]}.
        [[nodiscard]] json::Value to_json() const
            requires json::Writable<T>
        {
            if (ok())
            {
                return json::object({
                    {"ok", true},
                    {"value", std::get<0>(m_outcome).to_json()},
                    {"log", json::to_array(m_log)},
                });
            }
            return json::object({
                {"ok", false},
                {"error", std::get<1>(m_outcome).to_json()},
                {"log", json::to_array(m_log)},
            });
        }

    private:
        void require_ok() const
        {
            if (!ok())
            {
                const FileError& stored = std::get<1>(m_outcome);
                throw ConfigError("FileResult holds an error from " + stored.operation + " on " + stored.source +
                                  ": " + stored.message);
            }
        }

        std::variant<T, FileError> m_outcome;
        std::vector<LogMessage> m_log;
    };

    /// Runs `operation` and returns its result, or the FileError describing
    /// whatever it threw. Nothing escapes: every exception, library or not,
    /// becomes a FileError attributed to `operation_name` and `source`. What
    /// FFmpeg logs on this thread meanwhile, at Info level and above, is
    /// collected by a LogCapture into the result's `log()`. Info is included
    /// because decoders report concealed damage there.
    template <typename Operation>
    [[nodiscard]] auto capture(std::string operation_name, const Source& source, Operation&& operation)
        -> FileResult<std::invoke_result_t<Operation>>
    {
        using Value = std::invoke_result_t<Operation>;
        LogCapture log_capture(LogLevel::Info);
        try
        {
            Value value = std::forward<Operation>(operation)();
            return FileResult<Value>(std::move(value), log_capture.take());
        }
        catch (...)
        {
            FileError error = FileError::from_current_exception(std::move(operation_name), source.describe());
            return FileResult<Value>(std::move(error), log_capture.take());
        }
    }
}
