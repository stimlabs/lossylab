#pragma once

#include "lossylab/core/availability.hpp"
#include "lossylab/core/color_spec.hpp"
#include "lossylab/core/json.hpp"
#include "lossylab/core/pixel_format.hpp"
#include "lossylab/core/rational.hpp"
#include "lossylab/core/reflect.hpp"
#include "lossylab/io/icc_profile.hpp"
#include "lossylab/io/source.hpp"

#include <array>
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

    LOSSYLAB_REFLECT(ImageContainerInfo, has_alpha, is_animated, compression, frame_count, canvas_width,
                      canvas_height, is_still_image);

    /// What a JPEG file's markers declare: how it was coded and with which
    /// tables. FFmpeg decodes JPEG without reporting any of it, and it is most
    /// of what a JPEG says about the encoder that wrote it and how hard that
    /// encoder compressed.
    struct JpegInfo
    {
        /// From the start-of-frame marker: "baseline", "extended",
        /// "progressive", "lossless", or "hierarchical".
        std::string process;
        bool arithmetic_coding = false;

        /// Sample precision in bits: 8, or 12 for extended JPEG.
        int precision = 8;

        struct Component
        {
            int id = 0;
            int horizontal_sampling = 1;
            int vertical_sampling = 1;
            int quantization_table = 0;
        };
        std::vector<Component> components;

        struct QuantizationTable
        {
            int id = 0;

            /// 8, or 16 for tables stored with 16-bit entries.
            int precision = 8;

            /// In natural (row-major) order, not the file's zigzag order.
            std::array<int, 64> values{};
        };

        /// Every table the file defines, in file order. A later table with
        /// an id already seen replaces it for the scans that follow.
        std::vector<QuantizationTable> quantization_tables;

        /// The libjpeg quality setting (1-100) whose scaled standard tables
        /// come closest to this file's: table 0 against the luminance
        /// table, table 1 against the chrominance table.
        std::optional<int> ijg_quality;

        /// True when the tables are exactly libjpeg's at `ijg_quality`, as
        /// written by libjpeg, libjpeg-turbo and everything built on them
        /// (Pillow, OpenCV, most web tools). False means another encoder's
        /// tables, for which `ijg_quality` is only the nearest equivalent.
        bool ijg_quality_exact = false;

        /// "standard" when every Huffman table is one of the JPEG standard's
        /// example tables, which libjpeg writes unless asked to optimize;
        /// "custom" when none is, as with optimized or progressive output;
        /// "mixed"; or "none" when the file defines none.
        std::string huffman_tables;

        /// MCUs between restart markers; 0 when there are none.
        int restart_interval = 0;

        /// 1 for a sequential JPEG. A progressive JPEG's count follows its
        /// encoder's progression script.
        int scan_count = 0;

        /// An APPn or COM segment, in file order.
        struct Segment
        {
            /// "APP0" to "APP15", or "COM".
            std::string marker;

            /// The signature an APPn payload starts with, e.g. "JFIF",
            /// "Exif", "ICC_PROFILE", "Adobe", "Photoshop 3.0"; empty when
            /// there is none.
            std::string identifier;

            std::int64_t size_bytes = 0;
        };
        std::vector<Segment> segments;

        /// The first COM segment's text. Encoders often sign here, e.g.
        /// "Lavc62.11.100", or gd's "CREATOR: gd-jpeg v1.0 (using IJG JPEG
        /// v80), quality = 90".
        std::optional<std::string> comment;

        /// The Adobe APP14 segment's color transform: 0 for none (RGB or
        /// CMYK), 1 for YCbCr, 2 for YCCK.
        std::optional<int> adobe_transform;

        /// False when the file ends before its end-of-image marker, as an
        /// interrupted download or upload leaves it.
        bool has_end_of_image = false;

        /// Bytes after the end-of-image marker, where some cameras and apps
        /// append data of their own.
        std::int64_t trailing_bytes = 0;

        [[nodiscard]] json::Value to_json() const;
    };

    LOSSYLAB_REFLECT(JpegInfo, process, arithmetic_coding, precision, components, quantization_tables, ijg_quality,
                      ijg_quality_exact, huffman_tables, restart_interval, scan_count, segments, comment,
                      adobe_transform, has_end_of_image, trailing_bytes);

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
        /// A rotation the container asks for but has not applied. Says
        /// nothing about mirroring; `orientation` does.
        std::optional<double> rotation;

        /// How the stored image must be turned to display upright, as an EXIF
        /// orientation: 1 upright, 2 mirrored left to right, 3 rotated 180
        /// degrees, 4 mirrored top to bottom, 5 transposed (mirrored along
        /// the main diagonal), 6 rotated 90 degrees clockwise, 7 transversed
        /// (mirrored along the other diagonal), 8 rotated 90 degrees
        /// counterclockwise. Decoding leaves the pixels as stored unless asked
        /// otherwise; see `DecodeImageOptions::orientation`.
        std::optional<int> orientation;

        /// Where `orientation` came from: "exif" for the Orientation tag of a
        /// JPEG's, PNG's or WebP's EXIF block; "irot_imir" for an AVIF/HEIF
        /// image's rotation and mirror properties, which take precedence over
        /// any EXIF there; "display_matrix" for a video track's matrix.
        std::string orientation_source;

        /// `Present` when the file declares an orientation. `NotPresent` when
        /// probe read where one would be and found none, or the demuxer
        /// reported none. `NotSupportedByBuild` for a still-image format whose
        /// orientation probe cannot read without decoding (JPEG XL, TIFF, and
        /// others FFmpeg reads with a bare image parser); decode_image()
        /// reports it for those.
        Availability orientation_availability = Availability::NotPresent;

        /// The embedded ICC profile, from a JPEG's APP2 segments, a PNG's
        /// iCCP chunk, a WebP's ICCP chunk, or an ISOBMFF colr box.
        std::optional<IccProfileInfo> icc_profile;

        /// As `orientation_availability`, for `icc_profile`.
        Availability icc_profile_availability = Availability::NotPresent;

        /// When the stream carries both an ICC profile and color tags naming
        /// its primaries or transfer (AVIF and PNG may carry both): whether
        /// the profile describes what the tags say. A disagreement means one
        /// of them is wrong, and different software will pick different ones.
        std::optional<bool> icc_matches_tagged_color;

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

        /// For a JPEG file, what its markers declare. Empty for every other
        /// format, including motion JPEG inside a video container.
        std::optional<JpegInfo> jpeg;

        [[nodiscard]] json::Value to_json() const;
    };

    LOSSYLAB_REFLECT(
        StreamInfo,
        index,
        type,
        codec_name,
        codec_long_name,
        profile,
        level,
        width,
        height,
        pixel_format,
        bit_depth,
        color,
        color_fully_tagged,
        frame_rate,
        average_frame_rate,
        time_base,
        sample_aspect_ratio,
        is_variable_frame_rate,
        rotation,
        orientation,
        orientation_source,
        orientation_availability,
        icc_profile,
        icc_profile_availability,
        icc_matches_tagged_color,
        frame_count,
        duration_us,
        bit_rate,
        metadata,
        has_hdr_metadata,
        is_default,
        is_dependent,
        image_container,
        jpeg
    );

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

        /// The grid image's orientation, from its irot and imir properties,
        /// with the meaning of `StreamInfo::orientation`. An iPhone photo
        /// taken in portrait carries it here, not on any stream.
        std::optional<int> orientation;

        /// The grid image's ICC profile.
        std::optional<IccProfileInfo> icc_profile;

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

    LOSSYLAB_REFLECT(TileGrid, id, is_primary, title, width, height, coded_width, coded_height, orientation,
                      icc_profile, tiles);

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

    LOSSYLAB_REFLECT(ProbeResult, format_name, format_long_name, duration_us, bit_rate, size_bytes, streams,
                      tile_grids, metadata, major_brand, compatible_brands, claimed_extension, format_mismatch);

    /// Reads container and stream properties without decoding.
    ///
    /// Cheap enough to run over a whole dataset, and the basis for deciding
    /// what to decode and how.
    [[nodiscard]] ProbeResult probe(const Source& source);
}
