#pragma once

#include "lossylab/core/color_spec.hpp"
#include "lossylab/core/pixel_format.hpp"
#include "lossylab/core/result.hpp"
#include "lossylab/core/strict.hpp"
#include "lossylab/io/source.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace lossylab
{
    /// What decode_image() does with the orientation a file declares (EXIF
    /// Orientation, or an AVIF/HEIF image's irot and imir properties).
    enum class OrientationHandling
    {
        /// Leave the pixels as stored. The record states the orientation, so
        /// a caller can apply it later or compare stored layouts.
        Report,

        /// Rotate and flip the pixels to display upright, exactly, and record
        /// the coordinate transform that did it.
        Apply
    };

    std::string to_string(OrientationHandling handling);
    OrientationHandling orientation_handling_from_string(std::string_view name);

    /// How a decoded image should be delivered.
    struct DecodeImageOptions
    {
        /// Target pixel format. Left unset, the frame arrives in the codec's
        /// native format, which is what an audit wants: the chroma planes as
        /// the encoder actually wrote them, not an RGB rendering of them.
        std::optional<PixelFormat> pixel_format;

        /// Target color. Left unset, the frame keeps whatever the file tagged.
        std::optional<ColorSpec> color;

        /// Color to assume for whatever the file leaves unspecified. Files
        /// without color tags are common, and every decoder guesses
        /// differently; stating the assumption here keeps that guess out of
        /// the library. An embedded ICC profile that matches a known
        /// primaries and transfer pair (Display P3, sRGB, BT.709, ...) is used
        /// before this, and recorded as the source of those fields.
        ColorSpec assumed_color = ColorSpec::srgb();

        /// Leaves the pixels as stored by default, like PIL's Image.open; a
        /// training pipeline that wants what a viewer shows asks for Apply.
        OrientationHandling orientation = OrientationHandling::Report;

        /// Applies to the conversion, when one was requested, to the chroma
        /// layout change applying an orientation can entail, and to an ICC
        /// profile that cannot be expressed as color tags when a target color
        /// is requested.
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
    /// and any conversion applied afterwards. Its params carry the embedded
    /// ICC profile (`icc_profile`), the declared orientation (`orientation`,
    /// an EXIF value) and what was done with it (`orientation_handling`:
    /// "reported", "applied", or "applied_by_decoder" for JPEG XL, whose
    /// decoder turns the image upright itself).
    [[nodiscard]] FrameResult decode_image(const Source& source,
                                           const DecodeImageOptions& options = {});
}
