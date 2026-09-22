#pragma once

#include "lossylab/core/color_spec.hpp"
#include "lossylab/core/pixel_format.hpp"
#include "lossylab/core/result.hpp"
#include "lossylab/core/strict.hpp"
#include "lossylab/io/source.hpp"

#include <optional>

namespace lossylab
{
    /// How a decoded image should be delivered.
    struct DecodeImageOptions
    {
        /// Target pixel format. Left unset, the frame arrives in the codec's
        /// native format, which is what an audit wants: the chroma planes as
        /// the encoder actually wrote them, not an RGB rendering of them.
        std::optional<PixelFormat> pixel_format;

        /// Target color. Left unset, the frame keeps whatever the file tagged.
        std::optional<ColorSpec> color;

        /// Color to assume when the file tags none. Files without color tags
        /// are common, and every decoder guesses differently; stating the
        /// assumption here keeps that guess out of the library.
        ColorSpec assumed_color = ColorSpec::srgb();

        /// Applies to the conversion, when one was requested.
        Strict strict = Strict::AllowRecorded;
    };

    /// Decodes a still image.
    ///
    /// Covers the formats outside a typical PIL setup, gives access to native
    /// planes for chroma inspection, and keeps the decode path identical to the
    /// one production uses. JPEG stays with PIL by design; FFmpeg's MJPEG
    /// decoder is available here as a second implementation when comparing the
    /// two is the point.
    ///
    /// The record states the codec that decoded it, the format it arrived in,
    /// and any conversion applied afterwards.
    [[nodiscard]] FrameResult decode_image(const Source& source,
                                           const DecodeImageOptions& options = {});
}
