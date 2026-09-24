#pragma once

#include "lossylab/core/color_spec.hpp"
#include "lossylab/core/pixel_format.hpp"
#include "lossylab/core/reflect.hpp"
#include "lossylab/core/result.hpp"
#include "lossylab/core/strict.hpp"
#include "lossylab/io/probe.hpp"
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
        /// the library. Before this, what the codec itself fixes but FFmpeg's
        /// decoder leaves unset is filled in (lossy WebP's centered chroma),
        /// and an embedded ICC profile that matches a known primaries and
        /// transfer pair (Display P3, sRGB, BT.709, ...) is used; each is
        /// recorded as the source of the fields it filled.
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

    LOSSYLAB_REFLECT(DecodeImageOptions, pixel_format, color, assumed_color, orientation, strict);

    /// What decode_image() produced: the picture, the record of how, and
    /// everything probe() reports about the file.
    struct DecodedImage
    {
        /// probe()'s result for the same file, completed with what only
        /// decoding reveals: the orientation and ICC profile of a format read
        /// with a bare image parser, which probe reports as
        /// `NotSupportedByBuild`. Where the decoder and the file's own
        /// metadata disagree on either, this holds what the decoder used.
        ProbeResult probe;

        Frame frame;
        StageRecord record;

        /// The stream that was decoded; for a tile grid, its first tile's.
        [[nodiscard]] const StreamInfo& stream() const;

        /// The grid that was assembled, or nullptr for a single image.
        [[nodiscard]] const TileGrid* tile_grid() const;

        [[nodiscard]] json::Value to_json() const;
    };

    LOSSYLAB_REFLECT(DecodedImage, probe, frame, record);

    /// Decodes a still image.
    ///
    /// Covers the formats outside a typical PIL setup, gives access to native
    /// planes for chroma inspection, and keeps the decode path identical to the
    /// one production uses. JPEG stays with PIL by design; FFmpeg's MJPEG
    /// decoder is available here as a second implementation when comparing the
    /// two is the point.
    ///
    /// The file is opened once, for both the probe and the decode. The record
    /// states the codec that decoded it, the format it arrived in, and any
    /// conversion applied afterwards. Its params carry what decoding decided,
    /// never what the file declares, which is in `probe`: the stream decoded
    /// (`stream_index`), the tile grid assembled (`tile_grid_id`), the color
    /// assumed for untagged fields (`assumed_color`), and what was done with
    /// the orientation (`orientation_handling`: "reported", "applied", or
    /// "applied_by_decoder" for JPEG XL, whose decoder turns the image upright
    /// itself).
    [[nodiscard]] DecodedImage decode_image(const Source& source, const DecodeImageOptions& options = {});
}
