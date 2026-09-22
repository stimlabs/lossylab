#include "lossylab/pipeline/pipeline.hpp"

#include "lossylab/core/error.hpp"
#include "lossylab/env/capabilities.hpp"

#include <utility>

namespace lossylab
{
    struct Pipeline::Impl
    {
        PipelineSpec spec;
    };

    Pipeline::Pipeline(PipelineSpec spec) : m_impl(std::make_unique<Impl>())
    {
        // Validated at construction rather than at run time: a spec that this
        // build cannot execute should fail before a dataset starts, not on
        // sample forty thousand with half the data already written.
        spec.validate(capabilities());
        m_impl->spec = std::move(spec);
    }

    Pipeline::~Pipeline() = default;
    Pipeline::Pipeline(Pipeline&&) noexcept = default;
    Pipeline& Pipeline::operator=(Pipeline&&) noexcept = default;

    const PipelineSpec& Pipeline::spec() const noexcept
    {
        return m_impl->spec;
    }

    PipelineResult Pipeline::run(const std::vector<Frame>& frames, const std::uint64_t seed)
    {
        static_cast<void>(seed);
        if (frames.empty())
        {
            throw ConfigError("Pipeline::run() received no frames");
        }
        for (const Frame& frame : frames)
        {
            if (frame.empty())
            {
                throw ConfigError("Pipeline::run() received an empty frame");
            }
        }

        LL_NOT_IMPLEMENTED();
    }

    PipelineResult Pipeline::run(const Frame& frame, const std::uint64_t seed)
    {
        return run(std::vector<Frame>{frame}, seed);
    }
}
