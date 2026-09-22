#include "lossylab/core/strict.hpp"

#include "lossylab/core/error.hpp"

#include <utility>

namespace lossylab
{
    std::string to_string(const Strict mode)
    {
        return mode == Strict::Refuse ? "refuse" : "allow_recorded";
    }

    Strict strict_from_string(const std::string_view name)
    {
        if (name == "refuse") { return Strict::Refuse; }
        if (name == "allow_recorded") { return Strict::AllowRecorded; }
        throw ConfigError("unknown strict mode '" + std::string(name) + "'");
    }

    std::string to_string(const ConversionCause cause)
    {
        switch (cause)
        {
        case ConversionCause::Requested: return "requested";
        case ConversionCause::CodecConstraint: return "codec_constraint";
        case ConversionCause::GraphNegotiation: return "graph_negotiation";
        }
        return "unknown";
    }

    ConversionCause conversion_cause_from_string(const std::string_view name)
    {
        if (name == "requested") { return ConversionCause::Requested; }
        if (name == "codec_constraint") { return ConversionCause::CodecConstraint; }
        if (name == "graph_negotiation") { return ConversionCause::GraphNegotiation; }
        throw ConfigError("unknown conversion cause '" + std::string(name) + "'");
    }

    json::Value ConversionEvent::to_json() const
    {
        return json::object({
            {"property", property},
            {"from", from},
            {"to", to},
            {"cause", to_string(cause)},
            {"performed_by", performed_by},
        });
    }

    ConversionEvent ConversionEvent::from_json(const json::Value& value)
    {
        ConversionEvent event;
        event.property = value.at("property").get<std::string>();
        event.from = value.at("from").get<std::string>();
        event.to = value.at("to").get<std::string>();
        event.cause = conversion_cause_from_string(value.at("cause").get<std::string>());
        event.performed_by = json::string_or(value, "performed_by", "");
        return event;
    }

    bool operator==(const ConversionEvent& left, const ConversionEvent& right) noexcept
    {
        return left.property == right.property && left.from == right.from && left.to == right.to &&
               left.cause == right.cause && left.performed_by == right.performed_by;
    }

    void record_or_refuse(const Strict mode,
                          ConversionList& into,
                          std::string property,
                          std::string from,
                          std::string to,
                          const ConversionCause cause,
                          std::string performed_by,
                          const std::string_view context)
    {
        if (from == to)
        {
            // Not a conversion at all; nothing to refuse and nothing to record.
            return;
        }

        if (mode == Strict::Refuse)
        {
            throw ConversionRefused(property + " " + from, property + " " + to,
                                    std::string(context));
        }

        into.push_back(ConversionEvent{
            std::move(property), std::move(from), std::move(to), cause, std::move(performed_by),
        });
    }
}
