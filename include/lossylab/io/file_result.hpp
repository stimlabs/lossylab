#pragma once

#include "lossylab/core/error.hpp"
#include "lossylab/core/json.hpp"
#include "lossylab/core/json_io.hpp"
#include "lossylab/io/source.hpp"

#include <exception>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

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

    bool operator==(const FileError& left, const FileError& right) noexcept;
    inline bool operator!=(const FileError& left, const FileError& right) noexcept
    {
        return !(left == right);
    }

    /// The outcome of one operation on one input file: either its result or
    /// the FileError that replaced the exception it threw.
    template <typename T>
    class FileResult
    {
    public:
        explicit FileResult(T value) : m_outcome(std::in_place_index<0>, std::move(value)) {}
        explicit FileResult(FileError error) : m_outcome(std::in_place_index<1>, std::move(error)) {}

        [[nodiscard]] bool ok() const noexcept { return m_outcome.index() == 0; }

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

        /// {"ok": true, "value": ...} or {"ok": false, "error": ...}.
        [[nodiscard]] json::Value to_json() const
            requires json::Writable<T>
        {
            if (ok())
            {
                return json::object({{"ok", true}, {"value", std::get<0>(m_outcome).to_json()}});
            }
            return json::object({{"ok", false}, {"error", std::get<1>(m_outcome).to_json()}});
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
    };

    /// Runs `operation` and returns its result, or the FileError describing
    /// whatever it threw. Nothing escapes: every exception, library or not,
    /// becomes a FileError attributed to `operation_name` and `source`.
    template <typename Operation>
    [[nodiscard]] auto capture(std::string operation_name, const Source& source, Operation&& operation)
        -> FileResult<std::invoke_result_t<Operation>>
    {
        using Value = std::invoke_result_t<Operation>;
        try
        {
            return FileResult<Value>(std::forward<Operation>(operation)());
        }
        catch (...)
        {
            return FileResult<Value>(FileError::from_current_exception(std::move(operation_name), source.describe()));
        }
    }
}
