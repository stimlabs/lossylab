#pragma once

#include "lossylab/core/frame.hpp"
#include "lossylab/core/pixel_format.hpp"
#include "lossylab/core/result.hpp"
#include "lossylab/core/strict.hpp"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace lossylab
{
    /// How a graph's single input is configured. A filter graph cannot infer
    /// these, and guessing them is how a graph ends up silently converting.
    struct FilterInput
    {
        /// Name of the buffer source in the graph description, e.g. "in".
        std::string name = "in";

        int width = 0;
        int height = 0;
        PixelFormat pixel_format;
        ColorSpec color;
        Rational time_base{1, 25};
        Rational frame_rate{25, 1};
        Rational sample_aspect_ratio{1, 1};

        /// Builds an input description matching a frame.
        static FilterInput from_frame(const Frame& frame, std::string name = "in");
    };

    struct FilterGraphOptions
    {
        /// The graph description, in libavfilter's own syntax, e.g.
        /// "[in]gblur=sigma=2.0[out]".
        std::string description;

        std::vector<FilterInput> inputs;

        /// Pixel format the graph must deliver. The sink is constrained to it,
        /// so a graph that cannot produce it fails at compile time rather than
        /// silently converting at the end.
        PixelFormat output_pixel_format;

        /// Under Refuse, automatic format conversion inside the graph is
        /// disabled outright, so libavfilter reports a negotiation failure
        /// instead of inserting a scaler between two links. This is the
        /// setting that closes the hidden-conversion hole in filtering.
        Strict strict = Strict::Refuse;

        /// Pinned so that output does not depend on how many cores are free.
        /// Filters that thread can produce slightly different results at
        /// different thread counts.
        int thread_count = 1;
    };

    /// A compiled libavfilter graph.
    ///
    /// One escape hatch covering blur (including arbitrary point-spread
    /// functions, by convolving with a kernel image supplied as a second
    /// input), sharpening, denoising, color and LUT filters, overlays and text,
    /// padding and letterboxing, noise, and temporal filters. Wrapping each of
    /// those individually would be a large surface that libavfilter already
    /// provides; what the library adds is that the graph cannot convert behind
    /// your back and that what it did is recorded.
    ///
    /// Compilation is separated from execution because a randomized
    /// augmentation pipeline runs the same graph thousands of times with
    /// different parameters, and graph setup is not free.
    class FilterGraph
    {
    public:
        explicit FilterGraph(FilterGraphOptions options);
        ~FilterGraph();

        FilterGraph(const FilterGraph&) = delete;
        FilterGraph& operator=(const FilterGraph&) = delete;
        FilterGraph(FilterGraph&&) noexcept;
        FilterGraph& operator=(FilterGraph&&) noexcept;

        /// Runs frames through the graph.
        ///
        /// `auxiliary_inputs` supplies the additional input streams a
        /// multi-input graph needs, keyed by the input name: a PSF kernel image
        /// for convolution, an overlay asset, a second video for a difference
        /// filter.
        [[nodiscard]] FramesResult run(
            const std::vector<Frame>& frames,
            const std::map<std::string, std::vector<Frame>>& auxiliary_inputs = {});

        /// Runs a single frame through.
        [[nodiscard]] FrameResult run(const Frame& frame);

        /// Changes one parameter on the compiled graph.
        ///
        /// Uses libavfilter's runtime command interface where the filter
        /// supports it, and rebuilds the graph where it does not. Either way
        /// the caller gets the new value; `rebuild_count` says how often the
        /// expensive path was taken, which is worth watching when randomizing
        /// per sample.
        void set(const std::string& filter, const std::string& parameter,
                 const std::string& value);

        /// Whether the last `set` needed a rebuild.
        [[nodiscard]] bool last_set_rebuilt() const noexcept;
        [[nodiscard]] int rebuild_count() const noexcept;

        /// The graph as libavfilter describes it after compilation, including
        /// any conversion links it inserted. Under Strict::Refuse there are
        /// none, and this is how you verify that rather than assume it.
        [[nodiscard]] std::string describe() const;

        [[nodiscard]] const FilterGraphOptions& options() const noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };
}
