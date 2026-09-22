#include "lossylab/io/video_reader.hpp"

#include "lossylab/core/error.hpp"
#include "lossylab/core/json_io.hpp"

#include <utility>

namespace lossylab
{
    // -----------------------------------------------------------------------
    // FrameSelector
    // -----------------------------------------------------------------------

    /// Selection rules are held as data rather than as a closure wherever
    /// possible, so that a selector can be serialized into a pipeline spec and
    /// replayed later. `where` is the exception, and says so in its record.
    struct FrameSelector::Impl
    {
        enum class Kind
        {
            All,
            Indices,
            Stride,
            Timestamps,
            PictureTypes,
            EvenlySpaced,
            Predicate,
            Conjunction
        };

        Kind kind = Kind::All;
        std::vector<int> indices;
        std::vector<double> seconds;
        std::vector<PictureType> types;
        int stride = 1;
        int offset = 0;
        int count = 0;
        std::function<bool(const VideoFrame&)> predicate;
        std::shared_ptr<const Impl> left;
        std::shared_ptr<const Impl> right;
    };

    FrameSelector::FrameSelector(std::shared_ptr<const Impl> impl) : m_impl(std::move(impl)) {}

    json::Value FrameSelector::describe(const Impl& impl)
    {
        using Kind = Impl::Kind;

        switch (impl.kind)
        {
        case Kind::All:
            return json::object({{"select", "all"}});

        case Kind::Indices:
            return json::object(
                {{"select", "indices"}, {"indices", json::to_array(impl.indices)}});

        case Kind::Stride:
            return json::object(
                {{"select", "stride"}, {"step", impl.stride}, {"offset", impl.offset}});

        case Kind::Timestamps:
            return json::object(
                {{"select", "timestamps"}, {"seconds", json::to_array(impl.seconds)}});

        case Kind::PictureTypes:
            return json::object({
                {"select", "picture_types"},
                {"types", json::to_array(impl.types, [](const PictureType type) {
                     return to_string(type);
                 })},
            });

        case Kind::EvenlySpaced:
            return json::object({{"select", "evenly_spaced"}, {"count", impl.count}});

        case Kind::Predicate:
            // A closure cannot be serialized, so the record says the
            // selection is not replayable rather than pretending it is.
            return json::object({{"select", "predicate"}, {"replayable", false}});

        case Kind::Conjunction:
            return json::object({{"select", "and"},
                                 {"left", describe(*impl.left)},
                                 {"right", describe(*impl.right)}});
        }
        return json::Value();
    }

    FrameSelector FrameSelector::all()
    {
        return FrameSelector(std::make_shared<const Impl>());
    }

    FrameSelector FrameSelector::indices(std::vector<int> indices)
    {
        for (const int index : indices)
        {
            if (index < 0)
            {
                throw ConfigError("frame index cannot be negative");
            }
        }

        Impl impl;
        impl.kind = Impl::Kind::Indices;
        impl.indices = std::move(indices);
        return FrameSelector(std::make_shared<const Impl>(std::move(impl)));
    }

    FrameSelector FrameSelector::stride(const int step, const int offset)
    {
        if (step < 1)
        {
            throw ConfigError("frame stride must be at least 1");
        }
        if (offset < 0)
        {
            throw ConfigError("frame stride offset cannot be negative");
        }

        Impl impl;
        impl.kind = Impl::Kind::Stride;
        impl.stride = step;
        impl.offset = offset;
        return FrameSelector(std::make_shared<const Impl>(std::move(impl)));
    }

    FrameSelector FrameSelector::timestamps(std::vector<double> seconds)
    {
        Impl impl;
        impl.kind = Impl::Kind::Timestamps;
        impl.seconds = std::move(seconds);
        return FrameSelector(std::make_shared<const Impl>(std::move(impl)));
    }

    FrameSelector FrameSelector::picture_types(std::vector<PictureType> types)
    {
        if (types.empty())
        {
            throw ConfigError("picture_types() needs at least one type");
        }

        Impl impl;
        impl.kind = Impl::Kind::PictureTypes;
        impl.types = std::move(types);
        return FrameSelector(std::make_shared<const Impl>(std::move(impl)));
    }

    FrameSelector FrameSelector::evenly_spaced(const int count)
    {
        if (count < 1)
        {
            throw ConfigError("evenly_spaced() needs a positive count");
        }

        Impl impl;
        impl.kind = Impl::Kind::EvenlySpaced;
        impl.count = count;
        return FrameSelector(std::make_shared<const Impl>(std::move(impl)));
    }

    FrameSelector FrameSelector::where(std::function<bool(const VideoFrame&)> predicate)
    {
        if (!predicate)
        {
            throw ConfigError("where() needs a callable predicate");
        }

        Impl impl;
        impl.kind = Impl::Kind::Predicate;
        impl.predicate = std::move(predicate);
        return FrameSelector(std::make_shared<const Impl>(std::move(impl)));
    }

    FrameSelector FrameSelector::and_also(FrameSelector other) const
    {
        Impl impl;
        impl.kind = Impl::Kind::Conjunction;
        impl.left = m_impl;
        impl.right = std::move(other.m_impl);
        return FrameSelector(std::make_shared<const Impl>(std::move(impl)));
    }

    json::Value FrameSelector::to_json() const
    {
        return describe(*m_impl);
    }

    // -----------------------------------------------------------------------
    // VideoReader
    // -----------------------------------------------------------------------

    struct VideoReader::Impl
    {
        VideoReaderOptions options;
        StreamInfo stream;
        StageRecord record;
    };

    VideoReader::VideoReader(const Source& source, const VideoReaderOptions& options)
        : m_impl(std::make_unique<Impl>())
    {
        if (options.thread_count < 1)
        {
            throw ConfigError("VideoReader thread_count must be at least 1");
        }
        m_impl->options = options;

        // Probing first means an unreadable file fails here rather than on the
        // first frames() call, and gives stream() something to return.
        const ProbeResult probed = probe(source);
        const StreamInfo* stream = probed.primary_video_stream();
        if (stream == nullptr)
        {
            throw ConfigError("no video stream in " + source.describe());
        }
        m_impl->stream = *stream;

        LL_NOT_IMPLEMENTED();
    }

    VideoReader::~VideoReader() = default;
    VideoReader::VideoReader(VideoReader&&) noexcept = default;
    VideoReader& VideoReader::operator=(VideoReader&&) noexcept = default;

    const StreamInfo& VideoReader::stream() const noexcept
    {
        return m_impl->stream;
    }

    std::vector<VideoFrame> VideoReader::frames(const FrameSelector& select)
    {
        static_cast<void>(select);
        LL_NOT_IMPLEMENTED();
    }

    void VideoReader::for_each(const FrameSelector& select,
                               const std::function<bool(const VideoFrame&)>& callback)
    {
        static_cast<void>(select);
        static_cast<void>(callback);
        LL_NOT_IMPLEMENTED();
    }

    const StageRecord& VideoReader::record() const noexcept
    {
        return m_impl->record;
    }
}
