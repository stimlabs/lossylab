#pragma once

#include "lossylab/core/frame.hpp"
#include "lossylab/core/pipeline_spec.hpp"
#include "lossylab/core/result.hpp"
#include "lossylab/io/source.hpp"

#include <cstdint>
#include <memory>

namespace lossylab
{
    /// Executes a PipelineSpec on one image, in memory.
    ///
    /// Each stage runs the operation its configuration belongs to (decode,
    /// convert, chroma round trip, reinterpret, crop, orient, achromatic,
    /// image round trip), and the record collects every stage with the
    /// configuration it ran with, so `PipelineSpec::from_record()` of it runs
    /// the same chain again.
    class Pipeline
    {
    public:
        /// Validates the spec against this build.
        explicit Pipeline(PipelineSpec spec);
        ~Pipeline();

        Pipeline(const Pipeline&) = delete;
        Pipeline& operator=(const Pipeline&) = delete;
        Pipeline(Pipeline&&) noexcept;
        Pipeline& operator=(Pipeline&&) noexcept;

        /// Runs the chain on a file. The first stage must be a decode; when
        /// the spec names a source hash, `source` must have it.
        ///
        /// `seed` is recorded, and each stage that draws at random derives
        /// its own stream from it and its position, so one seed reproduces
        /// the whole run.
        [[nodiscard]] PipelineResult run(const Source& source, std::uint64_t seed = 0) const;

        /// Runs the chain on a frame. The first stage must not be a decode.
        [[nodiscard]] PipelineResult run(const Frame& frame, std::uint64_t seed = 0) const;

        [[nodiscard]] const PipelineSpec& spec() const noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };
}
