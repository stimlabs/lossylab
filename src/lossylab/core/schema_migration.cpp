#include "lossylab/core/schema_migration.hpp"

#include "lossylab/core/schema_version.hpp"

#include <utility>

namespace lossylab
{
    SchemaVersionError::SchemaVersionError(std::string type_name, const int version, const std::string& reason)
        : ConfigError(type_name + " with schema_version " + std::to_string(version) + ": " + reason),
          m_type_name(std::move(type_name)),
          m_version(version)
    {
    }

    SchemaMigrations::SchemaMigrations(std::string type_name, const int oldest_supported,
                                       std::vector<SchemaMigrationStep> steps)
        : m_type_name(std::move(type_name)), m_oldest_supported(oldest_supported), m_steps(std::move(steps))
    {
        const std::size_t expected_count = static_cast<std::size_t>(schema_version - oldest_supported);
        if (oldest_supported < 1 || oldest_supported > schema_version || m_steps.size() != expected_count)
        {
            throw Error(m_type_name + " declares " + std::to_string(m_steps.size()) + " schema migration steps from " +
                        std::to_string(oldest_supported) + ", but schema_version " + std::to_string(schema_version) +
                        " needs " + std::to_string(expected_count));
        }
        for (std::size_t i = 0; i < m_steps.size(); ++i)
        {
            if (m_steps[i].from_version != oldest_supported + static_cast<int>(i))
            {
                throw Error(m_type_name + " has a schema migration step from " +
                            std::to_string(m_steps[i].from_version) + " where " +
                            std::to_string(oldest_supported + static_cast<int>(i)) + " is next");
            }
        }
    }

    const json::Value& SchemaMigrations::upgrade(const json::Value& document, json::Value& storage) const
    {
        const auto stated = document.is_object() ? document.find("schema_version") : document.end();
        if (stated == document.end() || !stated->is_number_integer())
        {
            throw SchemaVersionError(m_type_name, 0, "the document states no integer schema_version");
        }
        const int version = stated->get<int>();
        if (version > schema_version)
        {
            throw SchemaVersionError(m_type_name, version,
                                     "written by a newer build; this one reads up to " +
                                         std::to_string(schema_version));
        }
        if (version < m_oldest_supported)
        {
            throw SchemaVersionError(m_type_name, version,
                                     "older than the oldest version still read, " +
                                         std::to_string(m_oldest_supported) + ", and not migrated");
        }

        if (version == schema_version)
        {
            return document;
        }

        storage = document;
        json::Value& upgraded = storage;
        for (int from = version; from < schema_version; ++from)
        {
            const SchemaMigrationStep& step = m_steps[static_cast<std::size_t>(from - m_oldest_supported)];
            if (step.apply != nullptr)
            {
                try
                {
                    step.apply(upgraded);
                }
                catch (const MigrationRefused& refusal)
                {
                    throw SchemaVersionError(m_type_name, version,
                                             "cannot be carried from schema " + std::to_string(from) + " to " +
                                                 std::to_string(from + 1) + " (" + step.summary + "): " +
                                                 refusal.what());
                }
            }
            upgraded["schema_version"] = from + 1;
        }
        return storage;
    }
}
