#include "lossylab/core/error.hpp"
#include "lossylab/core/record.hpp"
#include "lossylab/core/schema_migration.hpp"
#include "lossylab/core/schema_version.hpp"
#include "lossylab/io/file_result.hpp"
#include "lossylab/io/probe.hpp"

#include <cassert>
#include <string>

using namespace lossylab;

namespace
{
    void rename_old_field(json::Value& document)
    {
        document["new_field"] = document.at("old_field");
        document.erase("old_field");
    }

    void refuse_with_reason(json::Value&)
    {
        throw MigrationRefused("the old field has no counterpart");
    }

    /// The oldest layout a made-up output type reads is the one before the current schema.
    SchemaMigrations made_up_type(void (*apply)(json::Value&))
    {
        return SchemaMigrations("MadeUp", schema_version - 1, {{schema_version - 1, "old_field was renamed", apply}});
    }

    json::Value document_at(const int version)
    {
        return json::object({{"schema_version", version}, {"old_field", 7}});
    }

    json::Value upgraded_copy(const SchemaMigrations& migrations, const json::Value& document)
    {
        json::Value storage;
        return migrations.upgrade(document, storage);
    }

    void upgrade_without_rewrite(const json::Value& document)
    {
        static_cast<void>(upgraded_copy(made_up_type(nullptr), document));
    }

    void upgrade_refusing(const json::Value& document)
    {
        static_cast<void>(upgraded_copy(made_up_type(refuse_with_reason), document));
    }

    template <typename Callable>
    SchemaVersionError expect_refusal(Callable&& callable)
    {
        try
        {
            callable();
        }
        catch (const SchemaVersionError& error)
        {
            return error;
        }
        assert(false && "expected a SchemaVersionError");
        return SchemaVersionError("", 0, "");
    }

    void test_a_step_rewrites_the_document_and_moves_its_version_on()
    {
        const SchemaMigrations migrations = made_up_type(rename_old_field);
        const json::Value input = document_at(schema_version - 1);

        const json::Value upgraded = upgraded_copy(migrations, input);

        assert(upgraded.at("schema_version") == schema_version);
        assert(upgraded.at("new_field") == 7 && !upgraded.contains("old_field"));
        assert(input.at("schema_version") == schema_version - 1 && input.contains("old_field"));
    }

    void test_a_step_without_a_rewrite_only_moves_the_version_on()
    {
        const json::Value upgraded = upgraded_copy(made_up_type(nullptr), document_at(schema_version - 1));
        assert(upgraded.at("schema_version") == schema_version && upgraded.at("old_field") == 7);
    }

    void test_a_current_document_is_returned_without_a_copy()
    {
        const SchemaMigrations migrations = made_up_type(rename_old_field);
        const json::Value current = document_at(schema_version);
        json::Value storage;

        const json::Value& returned = migrations.upgrade(current, storage);

        assert(&returned == &current);
        assert(storage.is_null());
    }

    void test_an_older_document_is_returned_from_the_storage()
    {
        const SchemaMigrations migrations = made_up_type(rename_old_field);
        const json::Value older = document_at(schema_version - 1);
        json::Value storage;

        const json::Value& returned = migrations.upgrade(older, storage);

        assert(&returned == &storage);
    }

    void test_a_newer_document_is_refused()
    {
        const SchemaVersionError error =
            expect_refusal([] { upgrade_without_rewrite(document_at(schema_version + 1)); });
        assert(error.type_name() == "MadeUp" && error.version() == schema_version + 1);
    }

    void test_a_document_older_than_the_oldest_supported_is_refused()
    {
        const SchemaVersionError error =
            expect_refusal([] { upgrade_without_rewrite(document_at(schema_version - 2)); });
        assert(error.version() == schema_version - 2);
    }

    void test_a_document_without_a_version_is_refused()
    {
        expect_refusal([] { upgrade_without_rewrite(json::object({{"old_field", 7}})); });
        expect_refusal([] { upgrade_without_rewrite(document_at(1.5)); });
        expect_refusal([] { upgrade_without_rewrite(json::Value::array()); });
    }

    void test_a_step_that_refuses_names_the_type_the_versions_and_the_reason()
    {
        const SchemaVersionError error = expect_refusal([] { upgrade_refusing(document_at(schema_version - 1)); });
        const std::string message = error.what();
        assert(error.type_name() == "MadeUp");
        assert(message.find("old_field was renamed") != std::string::npos);
        assert(message.find("the old field has no counterpart") != std::string::npos);
    }

    void test_a_refusal_is_a_config_error()
    {
        bool caught_as_config_error = false;
        try
        {
            upgrade_refusing(document_at(schema_version - 1));
        }
        catch (const ConfigError&)
        {
            caught_as_config_error = true;
        }
        assert(caught_as_config_error);
    }

    void test_steps_must_cover_every_version_once_and_in_order()
    {
        const auto builds = [](const int oldest, std::vector<SchemaMigrationStep> steps)
        {
            try
            {
                const SchemaMigrations migrations("MadeUp", oldest, std::move(steps));
                return true;
            }
            catch (const Error&)
            {
                return false;
            }
        };

        assert(builds(schema_version, {}));
        assert(!builds(schema_version - 1, {}));
        assert(!builds(schema_version, {{schema_version, "beyond the current schema", nullptr}}));
        assert(!builds(schema_version - 1, {{schema_version - 2, "out of order", nullptr}}));
        assert(!builds(schema_version + 1, {}));
        assert(!builds(0, {}));
    }

    void test_every_output_type_covers_the_current_schema()
    {
        // Building them throws when a bump of schema_version left a type without its step.
        assert(ProcessingRecord::schema_migrations().oldest_supported() <= schema_version);
        assert(ProbeResult::schema_migrations().oldest_supported() <= schema_version);
        assert(FileError::schema_migrations().oldest_supported() <= schema_version);
    }
}

int main()
{
    test_a_step_rewrites_the_document_and_moves_its_version_on();
    test_a_step_without_a_rewrite_only_moves_the_version_on();
    test_a_current_document_is_returned_without_a_copy();
    test_an_older_document_is_returned_from_the_storage();
    test_a_newer_document_is_refused();
    test_a_document_older_than_the_oldest_supported_is_refused();
    test_a_document_without_a_version_is_refused();
    test_a_step_that_refuses_names_the_type_the_versions_and_the_reason();
    test_a_refusal_is_a_config_error();
    test_steps_must_cover_every_version_once_and_in_order();
    test_every_output_type_covers_the_current_schema();
    return 0;
}
