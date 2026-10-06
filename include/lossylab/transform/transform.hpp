#pragma once

#include "lossylab/core/frame.hpp"
#include "lossylab/core/geometry.hpp"
#include "lossylab/core/result.hpp"
#include "lossylab/transform/transform_types.hpp"

#include <optional>

namespace lossylab
{
    /// Cuts a rectangle out of the frame, copying the samples exactly. Every
    /// corner must fall on a whole chroma sample and byte of every plane.
    ///
    /// `block_grid` is the grid an earlier compression left in the input, as
    /// `ProcessingRecord::effective_block_grid()` gives it; the evidence then
    /// says where it falls in the crop.
    [[nodiscard]] FrameResult crop(const Frame& frame, const CropOptions& options,
                                   const std::optional<BlockGrid>& block_grid = std::nullopt);

    /// Rotates and flips the frame upright from `options.orientation`, moving
    /// every sample exactly. The output carries no orientation.
    [[nodiscard]] FrameResult orient(const Frame& frame, const OrientOptions& options);

    /// Sets R, G and B of every pixel of an rgb24 frame to its BT.601 luma,
    /// (19595 R + 38470 G + 7471 B + 32768) >> 16, libjpeg's fixed-point
    /// weights. JPEG's own conversion then gives the luma unchanged and flat
    /// chroma.
    [[nodiscard]] FrameResult achromatic(const Frame& frame, const AchromaticOptions& options = {});
}
