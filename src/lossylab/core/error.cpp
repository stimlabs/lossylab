#include "lossylab/core/error.hpp"

#include <utility>

namespace lossylab
{
    namespace
    {
        std::string unsupported_message(const std::string& kind,
                                        const std::string& name,
                                        const std::string& build_id)
        {
            return kind + " '" + name + "' is not available in this FFmpeg build (" + build_id + ")";
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
                                                 std::string build_id)
        : Error(unsupported_message(kind, name, build_id)),
          m_kind(std::move(kind)),
          m_name(std::move(name)),
          m_build_id(std::move(build_id))
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
