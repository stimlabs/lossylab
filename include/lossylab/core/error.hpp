#pragma once

#include <stdexcept>
#include <string>

namespace lossylab
{
    /// Base of every exception the library raises. Anything escaping the public
    /// API derives from this, so callers can catch one type at the boundary.
    class Error : public std::runtime_error
    {
    public:
        explicit Error(const std::string& what) : std::runtime_error(what) {}
    };

    /// Caller-supplied parameters are invalid or mutually inconsistent.
    class ConfigError : public Error
    {
    public:
        explicit ConfigError(const std::string& what) : Error(what) {}
    };

    /// A codec, filter, backend or hardware device the request needs is absent
    /// from the FFmpeg build in use. Carries the build's identity hash so a
    /// record or a bug report identifies which build refused.
    class UnsupportedCapability : public Error
    {
    public:
        UnsupportedCapability(std::string kind, std::string name, std::string identity_hash);

        /// "encoder", "decoder", "filter", "hwaccel", "backend", "muxer".
        [[nodiscard]] const std::string& kind() const noexcept { return m_kind; }
        [[nodiscard]] const std::string& name() const noexcept { return m_name; }
        [[nodiscard]] const std::string& identity_hash() const noexcept { return m_identity_hash; }

    private:
        std::string m_kind;
        std::string m_name;
        std::string m_identity_hash;
    };

    /// Strict mode refused an implicit conversion. This is the type that
    /// enforces the library's central promise: no format or color conversion
    /// happens unless it was asked for, and everything converted is recorded.
    class ConversionRefused : public Error
    {
    public:
        ConversionRefused(std::string from, std::string to, std::string context);

        [[nodiscard]] const std::string& from() const noexcept { return m_from; }
        [[nodiscard]] const std::string& to() const noexcept { return m_to; }
        [[nodiscard]] const std::string& context() const noexcept { return m_context; }

    private:
        std::string m_from;
        std::string m_to;
        std::string m_context;
    };

    /// An FFmpeg call returned an error. Wraps the AVERROR code together with
    /// av_strerror's text and the call site that produced it.
    class FFmpegError : public Error
    {
    public:
        FFmpegError(int averror, std::string call, std::string context);

        [[nodiscard]] int averror() const noexcept { return m_averror; }
        [[nodiscard]] const std::string& call() const noexcept { return m_call; }

    private:
        int m_averror;
        std::string m_call;
    };

    /// Scaffolding marker: the symbol exists with its final signature, but the
    /// body is not written yet. Carries the symbol name so a test run over the
    /// skeleton reports precisely what is still missing.
    class NotImplemented : public Error
    {
    public:
        explicit NotImplemented(std::string symbol);

        [[nodiscard]] const std::string& symbol() const noexcept { return m_symbol; }

    private:
        std::string m_symbol;
    };
}

/// Marks an unimplemented body. Used in place of a real implementation so the
/// declared API surface compiles and links while it is being filled in.
#define LL_NOT_IMPLEMENTED() throw ::lossylab::NotImplemented(__func__)
