#pragma once

#include "lossylab/core/color_spec.hpp"
#include "lossylab/core/json.hpp"
#include "lossylab/core/pixel_format.hpp"
#include "lossylab/core/reflect.hpp"
#include "lossylab/core/strict.hpp"

#include <optional>
#include <string>
#include <vector>

namespace lossylab
{
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

        /// Applies to the conversion, when one was requested.
        Strict strict = Strict::AllowRecorded;
    };

    LOSSYLAB_REFLECT(VideoReaderOptions, stream_index, pixel_format, color, assumed_color, export_qp_maps,
                      export_motion_vectors, thread_count, strict);

    /// What one read of a VideoReader was told: its options, with the stream
    /// resolved, and the frames it picked. Reading again with
    /// `FrameSelector::indices(frame_indices)` picks the same frames, whatever
    /// selector picked them first.
    struct DecodeVideoConfiguration
    {
        VideoReaderOptions reader;

        /// The positions of the frames delivered, as `VideoFrame::index`
        /// counts them, in the order they were delivered.
        std::vector<int> frame_indices;
    };

    LOSSYLAB_REFLECT(DecodeVideoConfiguration, reader, frame_indices);

    /// What one read of a VideoReader found in the file.
    struct DecodeVideoEvidence
    {
        /// "sha256:" and the SHA-256 of the file's bytes: the source's
        /// identity, which a replay checks it starts from.
        std::string source_sha256;

        /// The color the stream tags, and whether it tags every field.
        ColorSpec tagged_color;
        bool color_fully_tagged = false;

        /// How many frames were decoded to find the selected ones.
        int frames_decoded = 0;

        /// How the frames were picked (`FrameSelector::to_json()`); the
        /// configuration's `frame_indices` are what it picked.
        json::Value selector;
    };

    LOSSYLAB_REFLECT(DecodeVideoEvidence, source_sha256, tagged_color, color_fully_tagged, frames_decoded, selector);
}
