#pragma once

/// Reading image-container structure that FFmpeg's demuxers do not report.
/// Internal header.

#include "lossylab/io/source.hpp"

#include <cstdint>
#include <optional>

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
    };

    /// Walks the RIFF chunks of a WebP file, seeking past image data rather
    /// than reading it. Returns nullopt when the source is not a RIFF WEBP
    /// file.
    [[nodiscard]] std::optional<WebpChunks> read_webp_chunks(const Source& source);
}
