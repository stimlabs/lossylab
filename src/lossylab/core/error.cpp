#include "lossylab/core/error.hpp"

#include <utility>

namespace lossylab
{
    namespace
    {
        std::string unsupported_message(const std::string& kind,
                                        const std::string& name,
                                        const std::string& identity_hash)
        {
            return kind + " '" + name + "' is not available in this FFmpeg build (" + identity_hash + ")";
        }

        std::string refused_message(const std::string& from,
                                    const std::string& to,
                                    const std::string& context)
        {
            return "strict mode refused an implicit conversion from '" + from + "' to '" + to +
                   "' in " + context + "; request it explicitly or pass Strict::AllowRecorded";
        }
    }

    UnsupportedCapability::UnsupportedCapability(std::string kind,
                                                 std::string name,
                                                 std::string identity_hash)
        : Error(unsupported_message(kind, name, identity_hash)),
          m_kind(std::move(kind)),
          m_name(std::move(name)),
          m_identity_hash(std::move(identity_hash))
    {
    }

    UnsupportedCapability::UnsupportedCapability(std::string kind,
                                                 std::string name,
                                                 std::string identity_hash,
                                                 std::string message)
        : Error(std::move(message)),
          m_kind(std::move(kind)),
          m_name(std::move(name)),
          m_identity_hash(std::move(identity_hash))
    {
    }

    ConversionRefused::ConversionRefused(std::string from, std::string to, std::string context)
        : Error(refused_message(from, to, context)),
          m_from(std::move(from)),
          m_to(std::move(to)),
          m_context(std::move(context))
    {
    }

    FFmpegError::FFmpegError(const int averror, std::string call, std::string context)
        : Error(call + " failed: " + context),
          m_averror(averror),
          m_call(std::move(call))
    {
    }

    NotImplemented::NotImplemented(std::string symbol)
        : Error(symbol + " is declared but not implemented yet"),
          m_symbol(std::move(symbol))
    {
    }
}
