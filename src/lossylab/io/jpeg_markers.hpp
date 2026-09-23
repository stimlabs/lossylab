#pragma once

/// Reading a JPEG file's markers. Internal header.

#include "lossylab/io/probe.hpp"
#include "lossylab/io/source.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace lossylab::detail
{
    // Example quantization tables from the JPEG standard (ITU-T T.81, Annex
    // K), in natural order, as libjpeg and FFmpeg
    // (libavcodec/jpegquanttables.c) carry them.

    inline constexpr std::array<int, 64> standard_luminance_table = {
        16, 11, 10, 16, 24,  40,  51,  61,  12, 12, 14, 19, 26,  58,  60,  55,
        14, 13, 16, 24, 40,  57,  69,  56,  14, 17, 22, 29, 51,  87,  80,  62,
        18, 22, 37, 56, 68,  109, 103, 77,  24, 35, 55, 64, 81,  104, 113, 92,
        49, 64, 78, 87, 103, 121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99,
    };

    inline constexpr std::array<int, 64> standard_chrominance_table = {
        17, 18, 24, 47, 99, 99, 99, 99, 18, 21, 26, 66, 99, 99, 99, 99, 24, 26, 56, 99, 99, 99,
        99, 99, 47, 66, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
        99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
    };

    /// libjpeg's scaling of a standard table to a quality setting from 1 to
    /// 100 (jcparam.c, with force_baseline set, as its callers set it).
    [[nodiscard]] std::array<int, 64> ijg_scaled_table(const std::array<int, 64>& base, int quality);

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
