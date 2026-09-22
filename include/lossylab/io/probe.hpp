#pragma once

#include "lossylab/core/color_spec.hpp"
#include "lossylab/core/json.hpp"
#include "lossylab/core/pixel_format.hpp"
#include "lossylab/core/rational.hpp"
#include "lossylab/io/source.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace lossylab
{
    /// Properties specific to an image-container stream: WebP, and the
    /// AVIF/HEIF family (which share the ISOBMFF container with ordinary
    /// MP4/MOV, distinguished by their brand). Attached to `StreamInfo` only
    /// where these questions apply.
    struct ImageContainerInfo
    {
        /// True when the image carries alpha. For WebP this comes from the
        /// file's chunks, since FFmpeg decodes every lossless WebP to a format
        /// with an alpha plane. For AVIF/HEIF it comes from the stream's pixel
        /// format, which misses the usual case of alpha stored as a separate
        /// auxiliary image: FFmpeg reports that as another stream (see
        /// `ProbeResult::additional_images()`) without saying it is alpha.
        bool has_alpha = false;

        /// True when the container declares more than one frame: an animated
        /// WebP, or an AVIF/HEIF image sequence.
        bool is_animated = false;

        /// WebP only: "lossy" (VP8 image data), "lossless" (VP8L), or
        /// "mixed" for an animation whose frames use both. FFmpeg reports one
        /// codec for all of them.
        std::optional<std::string> compression;

        /// WebP only: the number of frames the file declares, 1 for a still.
        std::optional<std::int64_t> frame_count;

        /// WebP only: the extended header's canvas size, when the file has
        /// one. The only size available for an animated WebP, which FFmpeg's
        /// WebP reader cannot open, so its stream reports 0x0.
        std::optional<int> canvas_width;
        std::optional<int> canvas_height;

        /// True when an AVIF/HEIF file holds exactly one image rather than a
        /// sequence. Unset for WebP, where "still" isn't the complementary
        /// term to "animated" in the same way.
        std::optional<bool> is_still_image;

        [[nodiscard]] json::Value to_json() const;
    };

    /// One stream's properties, as the container and codec parameters describe
    /// them. Nothing here required decoding a single frame.
    struct StreamInfo
    {
        int index = 0;

        /// "video", "audio", "subtitle", "data", "attachment".
        std::string type;

        std::string codec_name;
        std::string codec_long_name;

        /// Profile and level as the bitstream declares them, e.g. "High" and
        /// 41 for H.264 High@4.1. Profiles identify encoder configurations and
        /// often the tool that produced a file.
        std::string profile;
        std::optional<int> level;

        int width = 0;
        int height = 0;
        PixelFormat pixel_format;
        int bit_depth = 0;

        /// Color as *tagged*, which is not necessarily the color as encoded.
        /// A file tagged BT.709 but graded as BT.601 is a real and common
        /// thing; this reports what the tags claim.
        ColorSpec color;

        /// True when the container or stream left color fields unspecified.
        /// Untagged color is itself informative: it points at the tools that
        /// produced the file.
        bool color_fully_tagged = false;

        Rational frame_rate{0, 1};
        Rational average_frame_rate{0, 1};
        Rational time_base{0, 1};
        Rational sample_aspect_ratio{1, 1};

        /// True when `average_frame_rate` differs from `frame_rate` by more
        /// than a small tolerance. `frame_rate` is the container's nominal
        /// rate; `average_frame_rate` is measured from actual frame timing.
        /// A stream re-timed by a frame-rate conversion, or one that was
        /// variable-frame-rate to begin with, shows a gap between the two.
        /// False when either rate is unknown, since a gap cannot be measured.
        bool is_variable_frame_rate = false;

        /// Display rotation in degrees from the display matrix side data.
        /// A rotation the container asks for but has not applied.
        std::optional<double> rotation;

        std::optional<std::int64_t> frame_count;
        std::optional<std::int64_t> duration_us;
        std::optional<std::int64_t> bit_rate;

        /// Stream-level metadata verbatim.
        std::map<std::string, std::string> metadata;

        /// True when the stream carries HDR mastering display or content light
        /// level side data.
        bool has_hdr_metadata = false;

        /// The container marks this stream as the one to present by default.
        /// In AVIF/HEIF, the primary image.
        bool is_default = false;

        /// The stream is only a part of something else. In AVIF/HEIF, one
        /// tile of a grid image; see `ProbeResult::tile_grids`.
        bool is_dependent = false;

        /// WebP- or AVIF/HEIF-specific properties. Empty for every other
        /// format, and for these formats when the build's demuxer did not
        /// expose enough to fill it in.
        std::optional<ImageContainerInfo> image_container;

        [[nodiscard]] json::Value to_json() const;
    };

    /// An image assembled from tiles, as AVIF/HEIF grid images are: iPhone
    /// photos, for instance, are 512x512 tiles. Each tile is its own stream.
    struct TileGrid
    {
        /// The container's identifier for the grid (the HEIF item id).
        std::int64_t id = 0;

        /// True when the grid is the file's primary image. A second grid is
        /// typically the alpha plane of the first, split into tiles the same
        /// way.
        bool is_primary = false;

        /// The grid's name as the file gives it, e.g. "Color" or "Alpha".
        std::string title;

        /// The assembled image's size, after cropping.
        int width = 0;
        int height = 0;

        /// The canvas the tiles are placed on, before cropping.
        int coded_width = 0;
        int coded_height = 0;

        struct Tile
        {
            int stream_index = 0;

            /// Position of the tile's top-left corner on the canvas.
            int x = 0;
            int y = 0;
        };
        std::vector<Tile> tiles;

        [[nodiscard]] json::Value to_json() const;
    };

    /// What `probe` found.
    ///
    /// The first thing an audit looks at, and the thing that says what decoding
    /// will do before any decoding happens. Encoder strings and container brands
    /// frequently identify the exact tool or platform that produced a file,
    /// which is the cheapest strong evidence available.
    struct ProbeResult
    {
        /// Container short name as FFmpeg names it, e.g. "mov,mp4,m4a,3gp,3g2,mj2".
        std::string format_name;
        std::string format_long_name;

        std::optional<std::int64_t> duration_us;
        std::optional<std::int64_t> bit_rate;
        std::optional<std::int64_t> size_bytes;

        std::vector<StreamInfo> streams;

        /// Grid images, in the order the container lists them.
        std::vector<TileGrid> tile_grids;

        /// Container-level metadata verbatim. The "encoder" and "handler_name"
        /// keys, and the ISOBMFF brands below, are where tool fingerprints live.
        std::map<std::string, std::string> metadata;

        /// ISOBMFF major brand and compatible brands, e.g. "isom", "mp42",
        /// "avc1". Platforms that re-encode uploads leave recognizable brand
        /// sets behind.
        std::string major_brand;
        std::vector<std::string> compatible_brands;

        /// The extension the source claimed for itself: a path's suffix, or a
        /// memory source's caller-supplied hint. Empty when the source gave
        /// neither. See `Source::claimed_extension()`.
        std::string claimed_extension;

        /// True when `claimed_extension` is non-empty and does not appear
        /// among the extensions FFmpeg's demuxer normally uses for
        /// `format_name`. The format itself always comes from sniffing the
        /// bytes, never from the extension, so this flags a disagreement
        /// between what the source claimed to be and what it actually is —
        /// evidence of conversion or mislabeling upstream, not a decoding
        /// failure.
        bool format_mismatch = false;

        /// The encoder string, from container or stream metadata, when present.
        [[nodiscard]] std::optional<std::string> encoder_string() const;

        /// The video stream to treat as the file's picture: the first one
        /// marked default, else the first that is not part of something else.
        /// Nullptr when there is none, which includes a file whose primary
        /// image is a tile grid: see `primary_tile_grid()`.
        [[nodiscard]] const StreamInfo* primary_video_stream() const noexcept;

        /// The grid that is the file's primary image, or nullptr.
        [[nodiscard]] const TileGrid* primary_tile_grid() const noexcept;

        /// In an AVIF/HEIF file, the images besides the primary one: its
        /// alpha plane, a depth or gain map, a thumbnail. Streams that are not
        /// the primary stream and not tiles, plus the grids that are not the
        /// primary grid. FFmpeg does not report which role each plays; its
        /// title (e.g. "Alpha"), size and pixel format are what there is.
        struct AdditionalImages
        {
            std::vector<int> stream_indices;
            std::vector<std::int64_t> tile_grid_ids;
        };
        [[nodiscard]] AdditionalImages additional_images() const;

        [[nodiscard]] json::Value to_json() const;
    };

    /// Reads container and stream properties without decoding.
    ///
    /// Cheap enough to run over a whole dataset, and the basis for deciding
    /// what to decode and how.
    [[nodiscard]] ProbeResult probe(const Source& source);
}
