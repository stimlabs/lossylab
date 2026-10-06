#pragma once

#include "lossylab/core/geometry.hpp"
#include "lossylab/core/reflect.hpp"
#include "lossylab/core/strict.hpp"

#include <optional>

namespace lossylab
{
    /// The rectangle to keep, in the input's pixels.
    struct CropOptions
    {
        int x = 0;
        int y = 0;
        int width = 0;
        int height = 0;
    };

    LOSSYLAB_REFLECT(CropOptions, x, y, width, height);

    /// What a crop found about the block grid an earlier compression left.
    struct CropEvidence
    {
        /// That grid in the crop's coordinates; nullopt when none was known.
        /// A nonzero phase means the crop does not start on a block corner.
        std::optional<BlockGrid> block_grid;

        /// True when the crop's width and height are whole blocks of that
        /// grid, so a rotation or flip afterwards keeps it starting on a
        /// block corner too.
        std::optional<bool> whole_blocks;
    };

    LOSSYLAB_REFLECT(CropEvidence, block_grid, whole_blocks);

    /// The EXIF orientation (1 to 8) to turn the frame upright from.
    struct OrientOptions
    {
        int orientation = 1;

        /// Applies to the 4:2:2/4:4:0 swap a transposing orientation entails.
        Strict strict = Strict::AllowRecorded;
    };

    LOSSYLAB_REFLECT(OrientOptions, orientation, strict);

    struct OrientEvidence
    {
        /// The orientation the frame itself carried, if any, which the one
        /// applied may disagree with.
        std::optional<int> frame_orientation;
    };

    LOSSYLAB_REFLECT(OrientEvidence, frame_orientation);

    /// Replaces every pixel of an rgb24 frame by its BT.601 luma, in all
    /// three channels.
    struct AchromaticOptions
    {
    };

    LOSSYLAB_REFLECT_EMPTY(AchromaticOptions);

    /// How far from gray the input was: the per-pixel spread, the largest
    /// channel minus the smallest, over every pixel.
    struct AchromaticEvidence
    {
        double channel_spread_mean = 0.0;

        /// Sample standard deviation (n - 1).
        double channel_spread_std = 0.0;
        int channel_spread_max = 0;
    };

    LOSSYLAB_REFLECT(AchromaticEvidence, channel_spread_mean, channel_spread_std, channel_spread_max);
}
