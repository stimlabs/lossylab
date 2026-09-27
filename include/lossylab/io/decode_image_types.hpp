#pragma once

#include "lossylab/core/color_spec.hpp"
#include "lossylab/core/pixel_format.hpp"
#include "lossylab/core/reflect.hpp"
#include "lossylab/core/strict.hpp"

#include <cstdint>
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

    /// For reflect::from_json().
    inline void from_string(const std::string_view name, OrientationHandling& value)
    {
        value = orientation_handling_from_string(name);
    }

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
        /// the library. Before this, what the codec itself fixes but FFmpeg's
        /// decoder leaves unset is filled in (lossy WebP's centered chroma),
        /// and an embedded ICC profile that matches a known primaries and
        /// transfer pair (Display P3, sRGB, BT.709, ...) is used; each is
        /// recorded as the source of the fields it filled.
        ColorSpec assumed_color = ColorSpec::srgb();

        /// Leaves the pixels as stored by default, like PIL's Image.open; a
        /// training pipeline that wants what a viewer shows asks for Apply.
        OrientationHandling orientation = OrientationHandling::Report;

        /// Applies to the conversion, when one was requested, and to the
        /// chroma layout change applying an orientation can entail.
        Strict strict = Strict::AllowRecorded;
    };

    LOSSYLAB_REFLECT(DecodeImageOptions, pixel_format, color, assumed_color, orientation, strict);

    /// What decode_image() decided for one file, never what the file
    /// declares, which is in the probe.
    struct DecodeImageEvidence
    {
        /// "sha256:" and the SHA-256 of the file's bytes: the source's
        /// identity, which a replay checks it starts from.
        std::string source_sha256;

        /// The stream decoded.
        int stream_index = 0;

        /// The tile grid assembled, if any.
        std::optional<std::int64_t> tile_grid_id;

        /// What was done with the orientation: "reported", "applied", or
        /// "applied_by_decoder" for JPEG XL, whose decoder turns the image
        /// upright itself.
        std::string orientation_handling;
    };

    LOSSYLAB_REFLECT(DecodeImageEvidence, source_sha256, stream_index, tile_grid_id, orientation_handling);
}
