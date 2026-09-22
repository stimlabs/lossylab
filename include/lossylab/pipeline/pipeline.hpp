#pragma once

#include "lossylab/core/frame.hpp"
#include "lossylab/core/pipeline_spec.hpp"
#include "lossylab/core/result.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace lossylab
{
    /// Executes a PipelineSpec.
    ///
    /// Multi-generation platform chains (resize, convert, encode, decode,
    /// repeat) run entirely in C++ with no trip out to NumPy between stages.
    /// The same machinery serves equalization: applying a capture-like history
    /// to synthetic images is just another spec, and because it is the same
    /// spec format, what was applied to each class is directly comparable.
    class Pipeline
    {
    public:
        explicit Pipeline(PipelineSpec spec);
        ~Pipeline();

        Pipeline(const Pipeline&) = delete;
        Pipeline& operator=(const Pipeline&) = delete;
        Pipeline(Pipeline&&) noexcept;
        Pipeline& operator=(Pipeline&&) noexcept;

        /// Runs the chain.
        ///
        /// `seed` drives every stochastic stage, each drawing from a stream
        /// derived from it and its own position, so one seed reproduces the
        /// whole run regardless of batch size or thread count.
        [[nodiscard]] PipelineResult run(const std::vector<Frame>& frames,
                                         std::uint64_t seed = 0);

        [[nodiscard]] PipelineResult run(const Frame& frame, std::uint64_t seed = 0);

        [[nodiscard]] const PipelineSpec& spec() const noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };
}
