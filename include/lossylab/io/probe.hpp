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
        /// True when the stream's pixel format carries an alpha plane.
        bool has_alpha = false;

        /// True when the container declares more than one frame. Applies to
        /// WebP (an animated WebP) and to an AVIF/HEIF image sequence.
        bool is_animated = false;

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

        /// WebP- or AVIF/HEIF-specific properties. Empty for every other
        /// format, and for these formats when the build's demuxer did not
        /// expose enough to fill it in.
        std::optional<ImageContainerInfo> image_container;

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

        /// The first video stream, or nullptr when there is none.
        [[nodiscard]] const StreamInfo* primary_video_stream() const noexcept;

        [[nodiscard]] json::Value to_json() const;
    };

    /// Reads container and stream properties without decoding.
    ///
    /// Cheap enough to run over a whole dataset, and the basis for deciding
    /// what to decode and how.
    [[nodiscard]] ProbeResult probe(const Source& source);
}
