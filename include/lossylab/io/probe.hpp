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
