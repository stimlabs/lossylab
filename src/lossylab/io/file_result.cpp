#include "lossylab/io/file_result.hpp"

#include "lossylab/core/schema_version.hpp"

#include <new>
#include <stdexcept>

namespace lossylab
{
    std::string to_string(const FileErrorKind kind)
    {
        switch (kind)
        {
        case FileErrorKind::Config: return "config";
        case FileErrorKind::UnsupportedCapability: return "unsupported_capability";
        case FileErrorKind::ConversionRefused: return "conversion_refused";
        case FileErrorKind::FFmpeg: return "ffmpeg";
        case FileErrorKind::NotImplemented: return "not_implemented";
        case FileErrorKind::Library: return "library";
        case FileErrorKind::OutOfMemory: return "out_of_memory";
        case FileErrorKind::Internal: return "internal";
        }
        return "unknown";
    }

    FileErrorKind file_error_kind_from_string(const std::string_view name)
    {
        if (name == "config") { return FileErrorKind::Config; }
        if (name == "unsupported_capability") { return FileErrorKind::UnsupportedCapability; }
        if (name == "conversion_refused") { return FileErrorKind::ConversionRefused; }
        if (name == "ffmpeg") { return FileErrorKind::FFmpeg; }
        if (name == "not_implemented") { return FileErrorKind::NotImplemented; }
        if (name == "library") { return FileErrorKind::Library; }
        if (name == "out_of_memory") { return FileErrorKind::OutOfMemory; }
        if (name == "internal") { return FileErrorKind::Internal; }
        throw ConfigError("unknown file error kind '" + std::string(name) + "'");
    }

    FileError FileError::from_current_exception(std::string operation, std::string source)
    {
        FileError error;
        error.operation = std::move(operation);
        error.source = std::move(source);

        // Most-derived types first: each Error subclass would also match Error.
        try
        {
            throw;
        }
        catch (const UnsupportedCapability& exception)
        {
            error.kind = FileErrorKind::UnsupportedCapability;
            error.message = exception.what();
            error.details = json::object({
                {"capability_kind", exception.kind()},
                {"name", exception.name()},
                {"identity_hash", exception.identity_hash()},
            });
        }
        catch (const ConversionRefused& exception)
        {
            error.kind = FileErrorKind::ConversionRefused;
            error.message = exception.what();
            error.details = json::object({
                {"from", exception.from()},
                {"to", exception.to()},
                {"context", exception.context()},
            });
        }
        catch (const FFmpegError& exception)
        {
            error.kind = FileErrorKind::FFmpeg;
            error.message = exception.what();
            error.details = json::object({
                {"averror", exception.averror()},
                {"call", exception.call()},
            });
        }
        catch (const NotImplemented& exception)
        {
            error.kind = FileErrorKind::NotImplemented;
            error.message = exception.what();
            error.details = json::object({{"symbol", exception.symbol()}});
        }
        catch (const ConfigError& exception)
        {
            error.kind = FileErrorKind::Config;
            error.message = exception.what();
        }
        catch (const Error& exception)
        {
            error.kind = FileErrorKind::Library;
            error.message = exception.what();
        }
        catch (const std::bad_alloc& exception)
        {
            error.kind = FileErrorKind::OutOfMemory;
            error.message = exception.what();
        }
        catch (const std::exception& exception)
        {
            error.kind = FileErrorKind::Internal;
            error.message = exception.what();
        }
        catch (...)
        {
            error.kind = FileErrorKind::Internal;
            error.message = "exception not derived from std::exception";
        }
        return error;
    }

    json::Value FileError::to_json() const
    {
        return json::object({
            {"schema_version", schema_version},
            {"kind", to_string(kind)},
            {"operation", operation},
            {"source", source},
            {"message", message},
            {"details", details},
        });
    }

    FileError FileError::from_json(const json::Value& value)
    {
        FileError error;
        error.kind = file_error_kind_from_string(value.at("kind").get<std::string>());
        error.operation = value.at("operation").get<std::string>();
        error.source = value.at("source").get<std::string>();
        error.message = value.at("message").get<std::string>();
        const json::Value details = json::member(value, "details");
        error.details = details.is_object() ? details : json::Value::object();
        return error;
    }

    bool operator==(const FileError& left, const FileError& right) noexcept
    {
        return left.kind == right.kind && left.operation == right.operation && left.source == right.source &&
               left.message == right.message && left.details == right.details;
    }
}
