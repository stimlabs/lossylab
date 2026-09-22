#pragma once

#include "lossylab/core/availability.hpp"
#include "lossylab/core/frame.hpp"
#include "lossylab/core/record.hpp"
#include "lossylab/core/result.hpp"
#include "lossylab/io/probe.hpp"
#include "lossylab/io/source.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace lossylab
{
    /// Motion vectors a decoder exported for a frame.
    struct MotionVector
    {
        int source_x = 0;
        int source_y = 0;
        int dest_x = 0;
        int dest_y = 0;
        int block_width = 0;
        int block_height = 0;

        /// Which reference the block came from: negative for a past reference,
        /// positive for a future one.
        int source_index = 0;
    };

    /// A decoded frame together with what the decoder reported about it.
    struct VideoFrame
    {
        Frame frame;

        /// Position in decode order.
        int index = 0;

        FrameStats stats;

        /// Per-block quantization parameters, when export was requested and
        /// the selected decoder supports it. The basis for crop-level
        /// severity rather than whole-frame severity.
        std::optional<QpMap> qp_map;

        /// States why `qp_map` is empty when it is: not requested or not
        /// present in this frame (`NotPresent`), or unavailable because the
        /// selected decoder cannot export it regardless of the frame
        /// (`NotSupportedByBuild`). Read this before treating an empty
        /// `qp_map` as "this frame has no quantization data" — a decoder
        /// that cannot export a map at all would otherwise look identical to
        /// one that exported an empty map.
        Availability qp_map_availability = Availability::NotPresent;

        /// Per-block motion, when export was enabled. Distinguishes real camera
        /// motion from synthesized motion, and shows where a codec spent bits.
        std::vector<MotionVector> motion_vectors;

        /// As `qp_map_availability`, for `motion_vectors`. An empty vector
        /// alone cannot distinguish "no motion vectors were exported" from
        /// "the decoder cannot export motion vectors for this codec."
        Availability motion_vector_availability = Availability::NotPresent;
    };

    /// Which frames to pull out of a clip.
    ///
    /// Sampling is deliberately expressive because training frames should be
    /// stratified rather than taken from wherever decoding happens to land.
    /// An I-frame and the B-frame after it differ enormously in quality, and a
    /// dataset that drew them unevenly across two classes would teach a
    /// detector the sampling rather than the signal.
    class FrameSelector
    {
    public:
        /// Every frame.
        static FrameSelector all();

        /// Specific positions in decode order.
        static FrameSelector indices(std::vector<int> indices);

        /// Every nth frame.
        static FrameSelector stride(int step, int offset = 0);

        /// Frames at or after the given presentation times, in seconds.
        static FrameSelector timestamps(std::vector<double> seconds);

        /// Only frames of these picture types.
        static FrameSelector picture_types(std::vector<PictureType> types);

        /// At most `count` frames, spread evenly across the clip.
        static FrameSelector evenly_spaced(int count);

        /// An arbitrary predicate over the decoded frame and its statistics.
        /// The escape hatch for selection rules the others do not cover, such
        /// as "frames whose mean QP exceeds 30".
        static FrameSelector where(std::function<bool(const VideoFrame&)> predicate);

        /// Both conditions must hold.
        [[nodiscard]] FrameSelector and_also(FrameSelector other) const;

        [[nodiscard]] json::Value to_json() const;

    private:
        struct Impl;

        explicit FrameSelector(std::shared_ptr<const Impl> impl);

        /// Recursive serializer. A member because Impl is private and a
        /// conjunction has to serialize its two halves.
        static json::Value describe(const Impl& impl);

        /// Shared rather than owned: selectors are small, immutable, and get
        /// combined with and_also(), so copying one should not copy its rules.
        std::shared_ptr<const Impl> m_impl;
    };

    struct VideoReaderOptions
    {
        /// Which stream to read. Negative means the first video stream.
        int stream_index = -1;

        /// Format to deliver frames in. Unset means the decoder's native
        /// format, which is what an audit wants.
        std::optional<PixelFormat> pixel_format;
        std::optional<ColorSpec> color;

        /// Color to assume when the file tags none.
        ColorSpec assumed_color = ColorSpec::bt709_limited();

        /// Ask the decoder for per-block quantizers. Not all decoders can, and
        /// it costs a little; off by default.
        bool export_qp_maps = false;

        /// Ask the decoder for motion vectors. Same caveats.
        bool export_motion_vectors = false;

        /// Pinned for reproducibility. Frame-threaded decoding can reorder
        /// side data even where the pixels come out identical.
        int thread_count = 1;

        Strict strict = Strict::Refuse;
    };

    /// Reads frames from a clip.
    ///
    /// The entry point both for sampling training frames out of real video and
    /// for auditing a video subset.
    class VideoReader
    {
    public:
        VideoReader(const Source& source, const VideoReaderOptions& options = {});
        ~VideoReader();

        VideoReader(const VideoReader&) = delete;
        VideoReader& operator=(const VideoReader&) = delete;
        VideoReader(VideoReader&&) noexcept;
        VideoReader& operator=(VideoReader&&) noexcept;

        /// Properties of the stream being read, without a second open.
        [[nodiscard]] const StreamInfo& stream() const noexcept;

        /// Decodes the selected frames.
        ///
        /// Selection is applied during decoding, so a selector that wants three
        /// frames from a long clip does not materialize the whole thing. Where
        /// the container allows it, seeking skips past what was not asked for.
        [[nodiscard]] std::vector<VideoFrame> frames(const FrameSelector& select);

        /// Streaming form, for clips too long to hold at once. The callback
        /// returns false to stop early.
        void for_each(const FrameSelector& select,
                      const std::function<bool(const VideoFrame&)>& callback);

        /// One record covering the whole read, with per-frame statistics.
        [[nodiscard]] const StageRecord& record() const noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };
}
