#pragma once

#include "lossylab/core/frame.hpp"
#include "lossylab/core/record.hpp"

#include <cstdint>
#include <vector>

namespace lossylab
{
    /// Every operation returns its output together with the record of what it
    /// did. Pairing them in the return type rather than through an out
    /// parameter is deliberate: it makes discarding the record a visible
    /// choice, and an unrecorded transformation is the thing this library is
    /// built to prevent.

    struct FrameResult
    {
        Frame frame;
        StageRecord record;
    };

    struct FramesResult
    {
        std::vector<Frame> frames;
        StageRecord record;
    };

    struct EncodedResult
    {
        std::vector<std::uint8_t> bytes;
        StageRecord record;

        /// Bits per pixel achieved, from the coded size and frame geometry.
        [[nodiscard]] double bits_per_pixel() const noexcept;
    };

    /// A pipeline's output: the frames, plus one record per stage.
    struct PipelineResult
    {
        std::vector<Frame> frames;
        ProcessingRecord record;
    };
}
