#pragma once

/// Reading a JPEG file's markers. Internal header.

#include "lossylab/io/probe.hpp"
#include "lossylab/io/source.hpp"

#include <optional>

namespace lossylab::detail
{
    /// Walks a JPEG file's markers from start to end, reading every segment's
    /// header and scanning past entropy-coded data. Returns nullopt when the
    /// source does not start with a JPEG start-of-image marker.
    [[nodiscard]] std::optional<JpegInfo> read_jpeg_markers(const Source& source);
}
