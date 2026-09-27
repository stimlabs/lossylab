#pragma once

#include "lossylab/core/color_spec.hpp"
#include "lossylab/core/pixel_format.hpp"
#include "lossylab/core/reflect.hpp"
#include "lossylab/core/result.hpp"
#include "lossylab/core/strict.hpp"
#include "lossylab/io/decode_image_types.hpp"
#include "lossylab/io/probe.hpp"
#include "lossylab/io/source.hpp"

#include <variant>

namespace lossylab
{
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

        /// Its evidence is a DecodeImageEvidence (see `evidence()`).
        StageRecord record;

        /// What the decode was told to do (see `FrameResult::configuration`).
        DecodeImageOptions configuration;

        /// What decoding decided: the source's hash, the stream decoded, the
        /// tile grid assembled, and what was done with the orientation.
        [[nodiscard]] const DecodeImageEvidence& evidence() const
        {
            return std::get<DecodeImageEvidence>(record.evidence);
        }

        /// The stream that was decoded; for a tile grid, its first tile's.
        [[nodiscard]] const StreamInfo& stream() const;

        /// The grid that was assembled, or nullptr for a single image.
        [[nodiscard]] const TileGrid* tile_grid() const;

        /// The start of the frame's processing history: this build's id,
        /// `probe` as its origin, and the decode as its first stage. Later
        /// stages are appended to it.
        [[nodiscard]] ProcessingRecord processing_record() const;

        [[nodiscard]] json::Value to_json() const;
    };

    LOSSYLAB_REFLECT(DecodedImage, probe, frame, record, configuration);

    /// Decodes a still image.
    ///
    /// Covers the formats outside a typical PIL setup, gives access to native
    /// planes for chroma inspection, and keeps the decode path identical to the
    /// one production uses. JPEG stays with PIL by design; FFmpeg's MJPEG
    /// decoder is available here as a second implementation when comparing the
    /// two is the point.
    ///
    /// The frame carries the file's ICC profile, the orientation it declares
    /// (unless applied), and its sample aspect ratio; see
    /// `Frame::icc_profile()`. The file is opened once, for both the probe
    /// and the decode. The record
    /// states the codec that decoded it, the format it arrived in, and any
    /// conversion applied afterwards. Its evidence carries what decoding
    /// decided, never what the file declares, which is in `probe`: the hash
    /// of the file's bytes (`source_sha256`), the stream decoded
    /// (`stream_index`), the tile grid assembled (`tile_grid_id`), and what
    /// was done with the orientation (`orientation_handling`: "reported",
    /// "applied", or "applied_by_decoder" for JPEG XL, whose decoder turns the
    /// image upright itself). The color assumed for untagged fields is a
    /// conversion in the record.
    [[nodiscard]] DecodedImage decode_image(const Source& source, const DecodeImageOptions& options = {});
}
