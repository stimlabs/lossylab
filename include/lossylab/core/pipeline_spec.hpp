#pragma once

#include "lossylab/core/json.hpp"
#include "lossylab/core/record.hpp"
#include "lossylab/core/stage_evidence.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lossylab
{
    class Capabilities;

    /// One stage of a pipeline: what it is told to do, as the same typed
    /// configuration a ProcessingRecord stores for it.
    struct StageSpec
    {
        StageConfiguration configuration;

        /// Optional label, to tell apart several stages of the same kind.
        std::string label;

        [[nodiscard]] StageKind kind() const noexcept { return stage_kind(configuration); }

        /// {"kind", "configuration", "label"}; the label only when set.
        [[nodiscard]] json::Value to_json() const;
        static StageSpec from_json(const json::Value& value);
    };

    /// A declarative, serializable pipeline: the stages to run on one file or
    /// frame, in order.
    ///
    /// Its stages are configurations of the kind a ProcessingRecord holds, so
    /// a record's configurations are a spec again (`from_record()`), and
    /// running a spec and replaying a record are the same thing.
    class PipelineSpec
    {
    public:
        PipelineSpec() = default;
        explicit PipelineSpec(std::vector<StageSpec> stages);

        PipelineSpec& add(StageSpec stage);
        PipelineSpec& add(StageConfiguration configuration, std::string label = {});

        [[nodiscard]] const std::vector<StageSpec>& stages() const noexcept { return m_stages; }
        [[nodiscard]] bool empty() const noexcept { return m_stages.empty(); }
        [[nodiscard]] std::size_t size() const noexcept { return m_stages.size(); }

        /// The SHA-256 of the file the spec was written for ("sha256:..."),
        /// as `Source::sha256()` gives it. When set, running the spec on any
        /// other file is refused.
        [[nodiscard]] const std::optional<std::string>& source_sha256() const noexcept { return m_source_sha256; }
        void set_source_sha256(std::optional<std::string> hash) { m_source_sha256 = std::move(hash); }

        /// Checks the spec before anything runs: there are stages, a decode
        /// comes only first, every kind can run in a pipeline, and the build
        /// has every encoder and backend a stage names. Throws ConfigError or
        /// UnsupportedCapability naming the offending stage by index.
        void validate(const Capabilities& capabilities) const;

        /// Validates against the current build.
        void validate() const;

        /// "sha256:" and the SHA-256 of `to_json()`. Two specs with the same
        /// id run the same operations, with the same options, in the same
        /// order, on the same file when `source_sha256` is set.
        [[nodiscard]] std::string spec_id() const;

        /// {"source_sha256", "stages"}.
        [[nodiscard]] json::Value to_json() const;
        static PipelineSpec from_json(const json::Value& value);

        /// Parses from a JSON document, as stored beside a dataset.
        static PipelineSpec parse(std::string_view text);

        /// The stages a record went through, as a spec that replays them: its
        /// configurations in order, and the source a first decode read.
        static PipelineSpec from_record(const ProcessingRecord& record);

    private:
        std::optional<std::string> m_source_sha256;
        std::vector<StageSpec> m_stages;
    };

    bool operator==(const PipelineSpec& left, const PipelineSpec& right);
    inline bool operator!=(const PipelineSpec& left, const PipelineSpec& right)
    {
        return !(left == right);
    }
}
