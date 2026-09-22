#include "lossylab/io/video_reader.hpp"

#include "lossylab/convert/convert.hpp"
#include "lossylab/core/error.hpp"
#include "lossylab/core/json_io.hpp"
#include "lossylab/detail/ff_error.hpp"
#include "lossylab/detail/ff_ptr.hpp"
#include "lossylab/env/build_info.hpp"
#include "lossylab/io/input_context.hpp"

extern "C" {
#include <libavutil/motion_vector.h>
}

#include <algorithm>
#include <chrono>
#include <string_view>
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
    // FrameSelector::Matcher
    // -----------------------------------------------------------------------

    namespace
    {
        /// `count` indices spread over `frame_count` frames, each at the
        /// center of its share of the clip.
        std::vector<int> evenly_spaced_indices(const int count, const std::int64_t frame_count)
        {
            std::vector<int> indices;
            if (frame_count <= 0)
            {
                return indices;
            }
            const std::int64_t taken = std::min<std::int64_t>(count, frame_count);
            for (std::int64_t i = 0; i < taken; ++i)
            {
                indices.push_back(static_cast<int>((2 * i + 1) * frame_count / (2 * taken)));
            }
            return indices;
        }
    }

    class FrameSelector::Matcher
    {
    public:
        /// `frame_count` is read only by `evenly_spaced` rules.
        Matcher(const Impl& rule, const std::int64_t frame_count) : m_rule(&rule)
        {
            using Kind = Impl::Kind;

            switch (rule.kind)
            {
            case Kind::Indices:
                m_indices = rule.indices;
                break;
            case Kind::EvenlySpaced:
                m_indices = evenly_spaced_indices(rule.count, frame_count);
                break;
            case Kind::Timestamps:
                m_pending_seconds = rule.seconds;
                std::sort(m_pending_seconds.begin(), m_pending_seconds.end());
                break;
            case Kind::Conjunction:
                m_left = std::make_unique<Matcher>(*rule.left, frame_count);
                m_right = std::make_unique<Matcher>(*rule.right, frame_count);
                break;
            case Kind::All:
            case Kind::Stride:
            case Kind::PictureTypes:
            case Kind::Predicate:
                break;
            }

            std::sort(m_indices.begin(), m_indices.end());
            m_indices.erase(std::unique(m_indices.begin(), m_indices.end()), m_indices.end());
        }

        [[nodiscard]] static bool needs_frame_count(const Impl& rule)
        {
            if (rule.kind == Impl::Kind::EvenlySpaced)
            {
                return true;
            }
            if (rule.kind == Impl::Kind::Conjunction)
            {
                return needs_frame_count(*rule.left) || needs_frame_count(*rule.right);
            }
            return false;
        }

        /// Whether `candidate` is selected. Has no effect on state; `commit`
        /// does that once the whole selector has matched.
        [[nodiscard]] bool matches(const VideoFrame& candidate) const
        {
            using Kind = Impl::Kind;

            switch (m_rule->kind)
            {
            case Kind::All:
                return true;

            case Kind::Indices:
            case Kind::EvenlySpaced:
                return std::binary_search(m_indices.begin(), m_indices.end(), candidate.index);

            case Kind::Stride:
                return candidate.index >= m_rule->offset && (candidate.index - m_rule->offset) % m_rule->stride == 0;

            case Kind::Timestamps:
            {
                const std::optional<double> seconds = candidate.frame.timestamp_seconds();
                return seconds.has_value() && m_next_pending < m_pending_seconds.size() &&
                       *seconds >= m_pending_seconds[m_next_pending];
            }

            case Kind::PictureTypes:
                return std::find(m_rule->types.begin(), m_rule->types.end(), candidate.stats.picture_type) !=
                       m_rule->types.end();

            case Kind::Predicate:
                return m_rule->predicate(candidate);

            case Kind::Conjunction:
                return m_left->matches(candidate) && m_right->matches(candidate);
            }
            return false;
        }

        /// Records that `selected` was delivered, retiring every pending
        /// timestamp it satisfies.
        void commit(const VideoFrame& selected)
        {
            if (m_rule->kind == Impl::Kind::Timestamps)
            {
                const std::optional<double> seconds = selected.frame.timestamp_seconds();
                while (seconds.has_value() && m_next_pending < m_pending_seconds.size() &&
                       m_pending_seconds[m_next_pending] <= *seconds)
                {
                    ++m_next_pending;
                }
            }
            else if (m_rule->kind == Impl::Kind::Conjunction)
            {
                m_left->commit(selected);
                m_right->commit(selected);
            }
        }

        /// True when no frame at `next_index` or later can match.
        [[nodiscard]] bool exhausted(const int next_index) const
        {
            using Kind = Impl::Kind;

            switch (m_rule->kind)
            {
            case Kind::Indices:
            case Kind::EvenlySpaced:
                return m_indices.empty() || next_index > m_indices.back();
            case Kind::Timestamps:
                return m_next_pending >= m_pending_seconds.size();
            case Kind::Conjunction:
                return m_left->exhausted(next_index) || m_right->exhausted(next_index);
            case Kind::All:
            case Kind::Stride:
            case Kind::PictureTypes:
            case Kind::Predicate:
                return false;
            }
            return false;
        }

    private:
        const Impl* m_rule;

        /// Sorted and unique, for `indices` and `evenly_spaced`.
        std::vector<int> m_indices;

        /// Sorted, for `timestamps`; those before `m_next_pending` are retired.
        std::vector<double> m_pending_seconds;
        std::size_t m_next_pending = 0;

        std::unique_ptr<Matcher> m_left;
        std::unique_ptr<Matcher> m_right;
    };

    // -----------------------------------------------------------------------
    // VideoReader
    // -----------------------------------------------------------------------

    namespace
    {
        /// Decoders that attach per-frame quantizer side data when asked to.
        constexpr std::string_view qp_exporting_decoders[] = {
            "h264",  "vp9",       "libdav1d",  "mpegvideo", "mpeg1video", "mpeg2video", "mpeg4",
            "h263",  "h263i",     "msmpeg4v1", "msmpeg4v2", "msmpeg4",    "wmv1",       "wmv2", "flv",
        };

        /// Decoders that attach motion vector side data when asked to.
        constexpr std::string_view motion_vector_exporting_decoders[] = {
            "h264", "mpegvideo", "mpeg1video", "mpeg2video", "mpeg4", "h263", "h263i",
            "msmpeg4v1", "msmpeg4v2", "msmpeg4", "wmv1", "wmv2", "flv",
        };

        template <std::size_t Size>
        bool contains(const std::string_view (&names)[Size], const std::string_view name)
        {
            return std::find(std::begin(names), std::end(names), name) != std::end(names);
        }

        /// Whether the decoder in use can export each kind of side data: known
        /// up front for the decoders listed above, and learned for any other
        /// decoder the first time it exports some.
        struct ExportSupport
        {
            bool qp_maps = false;
            bool motion_vectors = false;
        };

        Availability availability_of(const bool requested, const bool present, const bool decoder_exports)
        {
            if (present)
            {
                return Availability::Present;
            }
            if (!requested || decoder_exports)
            {
                return Availability::NotPresent;
            }
            return Availability::NotSupportedByBuild;
        }

        detail::CodecContextPtr open_video_decoder(const AVStream& stream, const VideoReaderOptions& options)
        {
            const AVCodec* codec = avcodec_find_decoder(stream.codecpar->codec_id);
            if (codec == nullptr)
            {
                const char* name = avcodec_get_name(stream.codecpar->codec_id);
                throw UnsupportedCapability("decoder", name != nullptr ? name : "unknown", build_info().build_id);
            }

            detail::CodecContextPtr context(LL_FF_ALLOC(avcodec_alloc_context3(codec)));
            LL_FF_CHECK(avcodec_parameters_to_context(context.get(), stream.codecpar));
            context->pkt_timebase = stream.time_base;
            context->thread_count = options.thread_count;
            if (options.export_qp_maps)
            {
                context->export_side_data |= AV_CODEC_EXPORT_DATA_VIDEO_ENC_PARAMS;
            }
            if (options.export_motion_vectors)
            {
                context->export_side_data |= AV_CODEC_EXPORT_DATA_MVS;
            }

            LL_FF_CHECK(avcodec_open2(context.get(), codec, nullptr));
            return context;
        }

        /// Number of packets the stream holds, read without decoding. Stands in
        /// for the frame count when the container does not declare one.
        std::int64_t count_packets(const Source& source, const int stream_index)
        {
            detail::InputContext input(source);
            AVFormatContext& format = *input.get();
            for (unsigned int index = 0; index < format.nb_streams; ++index)
            {
                if (static_cast<int>(index) != stream_index)
                {
                    format.streams[index]->discard = AVDISCARD_ALL;
                }
            }

            detail::PacketPtr packet = detail::make_packet();
            std::int64_t count = 0;
            while (true)
            {
                const int status = av_read_frame(&format, packet.get());
                if (status == AVERROR_EOF)
                {
                    break;
                }
                LL_FF_CHECK(status);
                if (packet->stream_index == stream_index)
                {
                    ++count;
                }
                av_packet_unref(packet.get());
            }
            return count;
        }

        /// The frame's motion vectors, or nullopt when the decoder attached
        /// none. An empty vector means the decoder attached an empty set.
        std::optional<std::vector<MotionVector>> motion_vectors_of(const AVFrame& raw)
        {
            const AVFrameSideData* side = av_frame_get_side_data(&raw, AV_FRAME_DATA_MOTION_VECTORS);
            if (side == nullptr)
            {
                return std::nullopt;
            }

            const auto* exported = reinterpret_cast<const AVMotionVector*>(side->data);
            const std::size_t count = side->size / sizeof(AVMotionVector);
            std::vector<MotionVector> vectors;
            vectors.reserve(count);
            for (std::size_t i = 0; i < count; ++i)
            {
                const AVMotionVector& vector = exported[i];
                vectors.push_back(MotionVector{vector.src_x, vector.src_y, vector.dst_x, vector.dst_y, vector.w,
                                               vector.h, vector.source});
            }
            return vectors;
        }

        void record_qp_statistics(const QpMap& map, FrameStats& stats)
        {
            if (map.values.empty())
            {
                return;
            }
            const auto [lowest, highest] = std::minmax_element(map.values.begin(), map.values.end());
            double sum = 0.0;
            for (const int value : map.values)
            {
                sum += value;
            }
            stats.qp_min = *lowest;
            stats.qp_max = *highest;
            stats.qp_mean = sum / static_cast<double>(map.values.size());
        }

        /// Wraps a decoded frame with its statistics and whatever side data the
        /// options asked for.
        VideoFrame assemble_video_frame(const AVFrame& raw, const int index, const Rational time_base,
                                        const VideoReaderOptions& options, ExportSupport& support)
        {
            VideoFrame video_frame;
            video_frame.frame = Frame::from_av_frame(&raw);
            video_frame.frame.set_time_base(time_base);
            if (raw.pts == AV_NOPTS_VALUE && raw.best_effort_timestamp != AV_NOPTS_VALUE)
            {
                video_frame.frame.set_pts(raw.best_effort_timestamp);
            }
            video_frame.index = index;

            FrameStats& stats = video_frame.stats;
            stats.index = index;
            stats.pts = video_frame.frame.pts();
            stats.picture_type = video_frame.frame.picture_type();
            stats.key_frame = video_frame.frame.is_key_frame();

            if (options.export_qp_maps)
            {
                video_frame.qp_map = video_frame.frame.qp_map();
                if (video_frame.qp_map.has_value())
                {
                    support.qp_maps = true;
                    record_qp_statistics(*video_frame.qp_map, stats);
                }
            }
            video_frame.qp_map_availability =
                availability_of(options.export_qp_maps, video_frame.qp_map.has_value(), support.qp_maps);

            std::optional<std::vector<MotionVector>> vectors;
            if (options.export_motion_vectors)
            {
                vectors = motion_vectors_of(raw);
                if (vectors.has_value())
                {
                    support.motion_vectors = true;
                    video_frame.motion_vectors = std::move(*vectors);
                }
            }
            video_frame.motion_vector_availability =
                availability_of(options.export_motion_vectors, vectors.has_value(), support.motion_vectors);

            return video_frame;
        }

        void record_once(ConversionList& conversions, const ConversionEvent& event)
        {
            if (std::find(conversions.begin(), conversions.end(), event) == conversions.end())
            {
                conversions.push_back(event);
            }
        }

        /// Fills in untagged color fields from `assumed`, recording the
        /// assumption once per distinct set of tags.
        void apply_assumed_color(Frame& frame, const ColorSpec& assumed_color, ConversionList& conversions)
        {
            const ColorSpec tagged = frame.color();
            if (tagged.is_fully_specified())
            {
                return;
            }
            const ColorSpec assumed = tagged.with_defaults_from(assumed_color);
            frame.set_color(assumed);
            frame.sync_color_to_av_frame();
            record_once(conversions, ConversionEvent{"color_tags", tagged.describe(), assumed.describe(),
                                                     ConversionCause::Requested, "assumed_color"});
        }
    }

    struct VideoReader::Impl
    {
        Source source;
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
        m_impl->source = source;
        m_impl->options = options;
        m_impl->record.kind = StageKind::Decode;

        // Probing first means an unreadable file fails here rather than on the
        // first frames() call, and gives stream() something to return.
        const ProbeResult probed = probe(source);
        const StreamInfo* stream = nullptr;
        if (options.stream_index < 0)
        {
            stream = probed.primary_video_stream();
            if (stream == nullptr)
            {
                throw ConfigError("no video stream in " + source.describe());
            }
        }
        else
        {
            for (const StreamInfo& candidate : probed.streams)
            {
                if (candidate.index == options.stream_index)
                {
                    stream = &candidate;
                }
            }
            if (stream == nullptr)
            {
                throw ConfigError("no stream " + std::to_string(options.stream_index) + " in " +
                                  source.describe());
            }
            if (stream->type != "video")
            {
                throw ConfigError("stream " + std::to_string(options.stream_index) + " in " + source.describe() +
                                  " is " + stream->type + ", not video");
            }
        }
        m_impl->stream = *stream;
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
        std::vector<VideoFrame> selected;
        read(select,
             [&](VideoFrame&& video_frame)
             {
                 selected.push_back(std::move(video_frame));
                 return true;
             });
        return selected;
    }

    void VideoReader::for_each(const FrameSelector& select,
                               const std::function<bool(const VideoFrame&)>& callback)
    {
        if (!callback)
        {
            throw ConfigError("for_each() needs a callable callback");
        }
        read(select, [&](VideoFrame&& video_frame) { return callback(video_frame); });
    }

    void VideoReader::read(const FrameSelector& select, const std::function<bool(VideoFrame&&)>& deliver)
    {
        const auto started = std::chrono::steady_clock::now();
        const VideoReaderOptions& options = m_impl->options;
        const StreamInfo& stream_info = m_impl->stream;
        const int stream_index = stream_info.index;

        std::int64_t frame_count = 0;
        if (FrameSelector::Matcher::needs_frame_count(*select.m_impl))
        {
            frame_count = stream_info.frame_count.value_or(0) > 0 ? *stream_info.frame_count
                                                                  : count_packets(m_impl->source, stream_index);
        }
        FrameSelector::Matcher matcher(*select.m_impl, frame_count);

        detail::InputContext input(m_impl->source);
        input.find_stream_info();
        AVFormatContext& format = *input.get();
        for (unsigned int index = 0; index < format.nb_streams; ++index)
        {
            if (static_cast<int>(index) != stream_index)
            {
                format.streams[index]->discard = AVDISCARD_ALL;
            }
        }
        const AVStream& stream = *format.streams[stream_index];
        const Rational time_base{stream.time_base.num, stream.time_base.den};

        detail::CodecContextPtr decoder = open_video_decoder(stream, options);
        const std::string decoder_name = decoder->codec->name != nullptr ? decoder->codec->name : "";

        ExportSupport support;
        support.qp_maps = contains(qp_exporting_decoders, decoder_name);
        support.motion_vectors = contains(motion_vector_exporting_decoders, decoder_name);

        const bool converting = options.pixel_format.has_value() || options.color.has_value();

        StageRecord record;
        record.kind = StageKind::Decode;
        record.implementation = decoder_name;
        record.transform = CoordinateTransform::identity();
        record.reproducible = options.thread_count == 1 || (!options.export_qp_maps && !options.export_motion_vectors);

        int decoded_count = 0;
        int selected_count = 0;

        // Returns false once reading should stop.
        const auto handle = [&](const AVFrame& raw) -> bool
        {
            if (matcher.exhausted(decoded_count))
            {
                return false;
            }
            const int index = decoded_count++;

            VideoFrame candidate = assemble_video_frame(raw, index, time_base, options, support);
            apply_assumed_color(candidate.frame, options.assumed_color, record.conversions);
            if (!matcher.matches(candidate))
            {
                return true;
            }
            matcher.commit(candidate);

            const FormatDescription native = candidate.frame.describe();
            if (converting)
            {
                ConvertOptions convert_options;
                convert_options.pixel_format = options.pixel_format.value_or(candidate.frame.pixel_format());
                convert_options.color = options.color.value_or(candidate.frame.color());
                convert_options.strict = options.strict;

                FrameResult converted = convert(candidate.frame, convert_options);
                for (const ConversionEvent& event : converted.record.conversions)
                {
                    record_once(record.conversions, event);
                }
                candidate.frame = std::move(converted.frame);
            }

            if (selected_count == 0)
            {
                record.input = native;
                record.output = candidate.frame.describe();
            }
            ++selected_count;
            record.frames.push_back(candidate.stats);

            const bool wants_more = deliver(std::move(candidate));
            return wants_more && !matcher.exhausted(index + 1);
        };

        detail::PacketPtr packet = detail::make_packet();
        detail::FramePtr decoded = detail::make_frame();

        // Hands every frame the decoder has ready to `handle`. Returns false
        // once reading should stop.
        const auto drain = [&]() -> bool
        {
            while (true)
            {
                const int received = avcodec_receive_frame(decoder.get(), decoded.get());
                if (received == AVERROR(EAGAIN) || received == AVERROR_EOF)
                {
                    return true;
                }
                LL_FF_CHECK(received);
                const bool keep_reading = handle(*decoded);
                av_frame_unref(decoded.get());
                if (!keep_reading)
                {
                    return false;
                }
            }
        };

        bool keep_reading = !matcher.exhausted(0);
        while (keep_reading)
        {
            const int status = av_read_frame(&format, packet.get());
            if (status == AVERROR_EOF)
            {
                break;
            }
            LL_FF_CHECK(status);

            if (packet->stream_index != stream_index)
            {
                av_packet_unref(packet.get());
                continue;
            }

            const int sent = avcodec_send_packet(decoder.get(), packet.get());
            av_packet_unref(packet.get());
            LL_FF_CHECK(sent);
            keep_reading = drain();
        }

        // A decoder holding frames back for reordering releases them only once
        // told there is no more input.
        if (keep_reading)
        {
            LL_FF_CHECK(avcodec_send_packet(decoder.get(), nullptr));
            drain();
        }

        if (selected_count == 0)
        {
            record.input =
                FormatDescription{stream_info.width, stream_info.height, stream_info.pixel_format, stream_info.color};
            record.output = record.input;
        }
        record.params = json::object({
            {"source", m_impl->source.describe()},
            {"codec", decoder_name},
            {"stream_index", stream_index},
            {"selector", select.to_json()},
            {"thread_count", options.thread_count},
            {"export_qp_maps", options.export_qp_maps},
            {"export_motion_vectors", options.export_motion_vectors},
            {"tagged_color", stream_info.color.to_json()},
            {"color_fully_tagged", stream_info.color_fully_tagged},
            {"assumed_color", options.assumed_color.to_json()},
            {"frames_decoded", decoded_count},
            {"frames_selected", selected_count},
        });
        record.duration_ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        m_impl->record = std::move(record);
    }

    const StageRecord& VideoReader::record() const noexcept
    {
        return m_impl->record;
    }
}
