#pragma once

#include "lossylab/core/error.hpp"
#include "lossylab/core/json.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace lossylab
{
    /// A stored document's `schema_version` is not one this build can load: it is newer than the build, older than
    /// the oldest version the output type still reads, or a migration step found data that no later layout can
    /// express. Derives from ConfigError, so callers that catch that keep working.
    class SchemaVersionError : public ConfigError
    {
    public:
        SchemaVersionError(std::string type_name, int version, const std::string& reason);

        /// The output type that refused, e.g. "ProcessingRecord".
        [[nodiscard]] const std::string& type_name() const noexcept { return m_type_name; }

        /// The `schema_version` the document states; the step it could not leave for a refused migration.
        [[nodiscard]] int version() const noexcept { return m_version; }

    private:
        std::string m_type_name;
        int m_version;
    };

    /// Thrown by a migration step that finds no meaningful way to carry its data to the next layout.
    /// `SchemaMigrations::upgrade()` turns it into a SchemaVersionError naming the type and the version.
    class MigrationRefused : public Error
    {
    public:
        explicit MigrationRefused(const std::string& reason) : Error(reason) {}
    };

    /// Rewrites a document from layout `from_version` to layout `from_version + 1`, in place. Refuses with
    /// `MigrationRefused` when the data cannot be carried over faithfully.
    struct SchemaMigrationStep
    {
        int from_version = 0;

        /// What changed between the two layouts, for the error and the docs.
        std::string summary;

        /// Null when the layout did not change for this output type, and only the version moves on.
        void (*apply)(json::Value& document) = nullptr;
    };

    /// How one output type upgrades the documents it wrote under an older `schema_version`.
    ///
    /// Every output type whose `to_json()` writes `schema_version` owns one of these, built once, and its loader
    /// starts with `upgrade()`. The steps must cover every version from `oldest_supported` up to the one before
    /// the current `schema_version`, each exactly once and in order. A version that carries nothing meaningful
    /// forward still gets a step, one that throws `MigrationRefused`, so that a bump of `schema_version` cannot go
    /// by without every output type deciding what it means for its stored data. Building the object with a gap
    /// throws, and so does the first load after a bump that adds no step.
    class SchemaMigrations
    {
    public:
        SchemaMigrations(std::string type_name, int oldest_supported, std::vector<SchemaMigrationStep> steps);

        [[nodiscard]] const std::string& type_name() const noexcept { return m_type_name; }
        [[nodiscard]] int oldest_supported() const noexcept { return m_oldest_supported; }
        [[nodiscard]] const std::vector<SchemaMigrationStep>& steps() const noexcept { return m_steps; }

        /// The document in the current layout, with `schema_version` set to the current one; the argument is
        /// untouched. Throws SchemaVersionError for a missing, non-integer, newer or unsupported version, and
        /// when a step refuses.
        [[nodiscard]] json::Value upgrade(const json::Value& document) const;

    private:
        std::string m_type_name;
        int m_oldest_supported;
        std::vector<SchemaMigrationStep> m_steps;
    };
}
