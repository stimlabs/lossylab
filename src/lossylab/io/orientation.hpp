#pragma once

/// Reading and applying an image's orientation. Internal header.

#include "lossylab/core/frame.hpp"
#include "lossylab/core/strict.hpp"

#include <cstdint>
#include <optional>
#include <span>

namespace lossylab::detail
{
    /// The Orientation tag (1-8) of an EXIF block, which starts with a TIFF
    /// header, optionally preceded by "Exif\0\0". Nullopt when the block has
    /// no such tag, holds a value outside 1-8, or cannot be parsed.
    [[nodiscard]] std::optional<int> exif_orientation(std::span<const std::uint8_t> exif);

    /// The EXIF orientation (1-8) a display matrix describes, from FFmpeg's
    /// AV_PKT_DATA_DISPLAYMATRIX / AV_FRAME_DATA_DISPLAYMATRIX side data.
    /// Nullopt when the data is too short, the matrix is singular, or its
    /// rotation is not a multiple of 90 degrees.
    [[nodiscard]] std::optional<int> orientation_from_display_matrix(std::span<const std::uint8_t> side_data);

    /// True when showing an image in this orientation swaps its width and
    /// height (orientations 5 to 8).
    [[nodiscard]] constexpr bool orientation_transposes(const int orientation) noexcept
    {
        return orientation >= 5 && orientation <= 8;
    }

    /// Rotates and flips `frame` so it displays upright, exactly: every
    /// sample moves, none is interpolated. Each chroma plane is flipped about
    /// its own edges, so along a subsampled axis of odd length the chroma
    /// ends up half a luma pixel from where it was. A transposing orientation
    /// swaps the chroma subsampling's axes, so 4:2:2 becomes 4:4:0; that change
    /// goes through `record_or_refuse` under `strict`. Throws NotImplemented
    /// for a format whose samples are not whole bytes or that has no
    /// transposed counterpart.
    [[nodiscard]] Frame apply_orientation(const Frame& frame, int orientation, Strict strict,
                                          ConversionList& conversions);
}
