#include "lossylab/core/availability.hpp"

#include "lossylab/core/error.hpp"

namespace lossylab
{
    std::string to_string(const Availability availability)
    {
        switch (availability)
        {
        case Availability::NotPresent: return "not_present";
        case Availability::NotSupportedByBuild: return "not_supported_by_build";
        case Availability::Present: return "present";
        }
        return "unknown";
    }

    Availability availability_from_string(const std::string_view name)
    {
        if (name == "not_present") { return Availability::NotPresent; }
        if (name == "not_supported_by_build") { return Availability::NotSupportedByBuild; }
        if (name == "present") { return Availability::Present; }
        throw ConfigError("unknown availability '" + std::string(name) + "'");
    }
}
