#pragma once

/// Reading image-container structure that FFmpeg's demuxers do not report.
/// Internal header.

#include "lossylab/io/source.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace lossylab::detail
{
    /// What a WebP file's RIFF chunks declare.
    struct WebpChunks
    {
        /// Whether any image data is coded lossy (a VP8 chunk) or lossless (a
        /// VP8L chunk). An animation can mix both.
        bool has_lossy = false;
        bool has_lossless = false;

        /// From the extended header's alpha flag, an ALPH chunk, or a VP8L
        /// header's alpha_is_used bit.
        bool has_alpha = false;

        bool is_animated = false;

        /// One per ANMF chunk for an animation; 1 otherwise.
        std::int64_t frame_count = 1;

        /// The extended (VP8X) header's canvas size, when there is one.
        std::optional<int> canvas_width;
        std::optional<int> canvas_height;

        /// The first ICCP and EXIF chunks' payloads, as the decoder takes
        /// them; later ones are ignored by both. An ICCP chunk too large to
        /// read leaves an empty profile, with the reason in `icc_problems`.
        std::optional<std::vector<std::uint8_t>> icc_profile;
        std::optional<std::vector<std::uint8_t>> exif;

        /// An ICCP chunk larger than the walker reads, or cut short.
        std::vector<std::string> icc_problems;
    };

    /// The most an embedded ICC profile or EXIF block is read into memory.
    /// Real profiles stay below a few megabytes.
    inline constexpr std::int64_t embedded_payload_limit = 16 * 1024 * 1024;

    /// Walks the RIFF chunks of a WebP file, seeking past image data rather
    /// than reading it. Returns nullopt when the source is not a RIFF WEBP
    /// file.
    [[nodiscard]] std::optional<WebpChunks> read_webp_chunks(const Source& source);

    /// What a PNG file's ancillary chunks hold that probe() interprets.
    struct PngChunks
    {
        /// The iCCP chunk's profile, decompressed, and the name it gives the
        /// profile (often a placeholder such as "ICC Profile"). A chunk that
        /// cannot be read leaves an empty profile, with the reason in
        /// `icc_problems`.
        std::optional<std::vector<std::uint8_t>> icc_profile;
        std::string icc_profile_name;
        std::vector<std::string> icc_problems;

        /// The eXIf chunk's payload.
        std::optional<std::vector<std::uint8_t>> exif;
    };

    /// Walks the chunks of a PNG file, seeking past image data. Returns
    /// nullopt when the source is not a PNG file.
    [[nodiscard]] std::optional<PngChunks> read_png_chunks(const Source& source);
}
