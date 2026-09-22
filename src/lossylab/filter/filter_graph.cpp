#include "lossylab/filter/filter_graph.hpp"

#include "lossylab/core/error.hpp"
#include "lossylab/env/capabilities.hpp"

#include <utility>

namespace lossylab
{
    struct FilterGraph::Impl
    {
        FilterGraphOptions options;
        int rebuild_count = 0;
        bool last_set_rebuilt = false;
    };

    FilterInput FilterInput::from_frame(const Frame& frame, std::string name)
    {
        if (frame.empty())
        {
            throw ConfigError("FilterInput::from_frame() received an empty frame");
        }

        FilterInput input;
        input.name = std::move(name);
        input.width = frame.width();
        input.height = frame.height();
        input.pixel_format = frame.pixel_format();
        input.color = frame.color();
        if (frame.time_base().is_valid() && frame.time_base().num != 0)
        {
            input.time_base = frame.time_base();
        }
        return input;
    }

    FilterGraph::FilterGraph(FilterGraphOptions options)
        : m_impl(std::make_unique<Impl>())
    {
        if (options.description.empty())
        {
            throw ConfigError("FilterGraph requires a graph description");
        }
        if (options.inputs.empty())
        {
            throw ConfigError("FilterGraph requires at least one input");
        }
        for (const FilterInput& input : options.inputs)
        {
            if (input.width <= 0 || input.height <= 0 || !input.pixel_format.is_valid())
            {
                throw ConfigError("FilterGraph input '" + input.name +
                                  "' is missing geometry or pixel format");
            }
            input.color.require_fully_specified("FilterGraph input '" + input.name + "'");
        }
        if (options.thread_count < 1)
        {
            throw ConfigError("FilterGraph thread_count must be at least 1");
        }

        m_impl->options = std::move(options);

        LL_NOT_IMPLEMENTED();
    }

    FilterGraph::~FilterGraph() = default;
    FilterGraph::FilterGraph(FilterGraph&&) noexcept = default;
    FilterGraph& FilterGraph::operator=(FilterGraph&&) noexcept = default;

    FramesResult FilterGraph::run(const std::vector<Frame>& frames,
                                  const std::map<std::string, std::vector<Frame>>& auxiliary_inputs)
    {
        static_cast<void>(frames);
        static_cast<void>(auxiliary_inputs);
        LL_NOT_IMPLEMENTED();
    }

    FrameResult FilterGraph::run(const Frame& frame)
    {
        static_cast<void>(frame);
        LL_NOT_IMPLEMENTED();
    }

    void FilterGraph::set(const std::string& filter, const std::string& parameter,
                          const std::string& value)
    {
        static_cast<void>(filter);
        static_cast<void>(parameter);
        static_cast<void>(value);
        LL_NOT_IMPLEMENTED();
    }

    bool FilterGraph::last_set_rebuilt() const noexcept
    {
        return m_impl->last_set_rebuilt;
    }

    int FilterGraph::rebuild_count() const noexcept
    {
        return m_impl->rebuild_count;
    }

    std::string FilterGraph::describe() const
    {
        LL_NOT_IMPLEMENTED();
    }

    const FilterGraphOptions& FilterGraph::options() const noexcept
    {
        return m_impl->options;
    }
}
