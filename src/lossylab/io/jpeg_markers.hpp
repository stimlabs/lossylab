#pragma once

/// Reading a JPEG file's markers. Internal header.

#include "lossylab/io/probe.hpp"
#include "lossylab/io/source.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace lossylab::detail
{
    /// What a JPEG file's markers hold: the coding parameters, plus the
    /// embedded data probe() interprets elsewhere.
    struct JpegMarkers
    {
        JpegInfo info;

        /// The ICC profile, reassembled from its APP2 segments in sequence
        /// order; empty when the file has none.
        std::vector<std::uint8_t> icc_profile;

        /// Missing, repeated or inconsistently numbered APP2 segments.
        std::vector<std::string> icc_problems;

        /// The first Exif APP1 segment's payload, "Exif\0\0" prefix included.
        /// Decoders ignore any later one, and so does this.
        std::optional<std::vector<std::uint8_t>> exif;
    };

    /// Walks a JPEG file's markers from start to end, reading every segment's
    /// header and scanning past entropy-coded data. Returns nullopt when the
    /// source does not start with a JPEG start-of-image marker.
    [[nodiscard]] std::optional<JpegMarkers> read_jpeg_markers(const Source& source);
}
