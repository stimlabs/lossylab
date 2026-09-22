#pragma once

#include "lossylab/core/json.hpp"
#include "lossylab/core/record.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace lossylab
{
    class Capabilities;

    /// One stage of a pipeline, as data.
    ///
    /// Parameters live in a json::Value rather than a variant of typed structs
    /// so that a spec can be written, logged, diffed and replayed without the
    /// C++ type system being in the way. They are validated against the
    /// stage's real options before anything runs.
    struct StageSpec
    {
        StageKind kind = StageKind::Convert;
        json::Value params;

        /// Optional label, carried into the record. Useful when a pipeline has
        /// several stages of the same kind and they need telling apart.
        std::string label;

        [[nodiscard]] json::Value to_json() const;
        static StageSpec from_json(const json::Value& value);
    };

    /// A declarative, serializable pipeline.
    ///
    /// This is the unit that gets logged alongside a sample, compared across
    /// classes, and replayed. Two datasets built from the same spec and the
    /// same seed received the same processing, and that claim is checkable
    /// rather than a matter of trust.
    class PipelineSpec
    {
    public:
        PipelineSpec() = default;
        explicit PipelineSpec(std::vector<StageSpec> stages);

        PipelineSpec& add(StageSpec stage);
        PipelineSpec& add(StageKind kind, json::Value params, std::string label = {});

        [[nodiscard]] const std::vector<StageSpec>& stages() const noexcept { return m_stages; }
        [[nodiscard]] bool empty() const noexcept { return m_stages.empty(); }
        [[nodiscard]] std::size_t size() const noexcept { return m_stages.size(); }

        /// Checks every stage against what the build can do, before running
        /// any of it.
        ///
        /// The alternative is discovering on sample 40,000 that one branch of a
        /// randomized pipeline needs an encoder this build lacks, by which
        /// point half a dataset has been built with a different distribution
        /// from the other half. Throws UnsupportedCapability or ConfigError
        /// naming the offending stage by index.
        void validate(const Capabilities& capabilities) const;

        /// Validates against the current build.
        void validate() const;

        /// A stable hash over the serialized spec. Two pipelines with the same
        /// id applied the same operations in the same order.
        [[nodiscard]] std::string spec_id() const;

        [[nodiscard]] json::Value to_json() const;
        static PipelineSpec from_json(const json::Value& value);

        /// Parses from a JSON document, as stored beside a dataset.
        static PipelineSpec parse(std::string_view text);

    private:
        std::vector<StageSpec> m_stages;
    };

    bool operator==(const PipelineSpec& left, const PipelineSpec& right);
    inline bool operator!=(const PipelineSpec& left, const PipelineSpec& right)
    {
        return !(left == right);
    }
}
