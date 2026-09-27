#pragma once

#include "lossylab/codec/encode_types.hpp"
#include "lossylab/core/codec_id.hpp"
#include "lossylab/core/frame.hpp"
#include "lossylab/core/json.hpp"
#include "lossylab/core/reflect.hpp"
#include "lossylab/core/result.hpp"
#include "lossylab/core/strict.hpp"

#include <cstdint>
#include <vector>

namespace lossylab
{
    /// Encodes frames to a byte stream.
    ///
    /// The record carries per-frame type, size and quantizer, the achieved bits
    /// per pixel, the fully resolved encoder settings, and the block grid the
    /// codec imposed. Frame i is given timestamp i at `frame_rate`, and the
    /// per-frame statistics are indexed by it. The quantizer is reported for
    /// x264, x265, libvpx-vp9 and SVT-AV1, in the scale the settings name
    /// under "qp_scale"; libaom and rav1e do not report one. The block grid
    /// is set where its edges are fixed: H.264 macroblocks, and every 64
    /// pixels for HEVC and VP9. FFmpeg's version strings are kept out of the
    /// output (AV_CODEC_FLAG_BITEXACT). Hardware backends are not implemented
    /// yet and throw NotImplemented.
    [[nodiscard]] EncodedResult encode_video(const std::vector<Frame>& frames,
                                             const EncodeVideoOptions& options);

    /// Encodes a single image.
    ///
    /// Covers the platform delivery formats. FFmpeg's MJPEG is available as a
    /// second JPEG implementation when encoder diversity is the point; PIL
    /// remains the primary JPEG path by design. The bytes are a complete
    /// file: JPEG, PNG, WebP, a JPEG XL codestream, a JP2 file (FFmpeg's own
    /// JPEG 2000 encoder), or AVIF muxed by FFmpeg.
    /// HEIF cannot be encoded, since FFmpeg has no HEIF muxer. The block grid
    /// is set for JPEG and JPEG XL (8x8) and lossy WebP (16x16 macroblocks).
    [[nodiscard]] EncodedResult encode_image(const Frame& frame,
                                             const EncodeImageOptions& options);

    /// Encode then decode, entirely in memory.
    ///
    /// The common augmentation case: no files, and no trip out to NumPy between
    /// the two halves. The record covers both, so a frame's compression history
    /// is one entry rather than two that have to be correlated afterwards: the
    /// encode's statistics, settings and block grid, both halves'
    /// conversions, and each half's evidence under `encode` and `decode`. The
    /// decoder assumes the encoded color for whatever the bitstream leaves
    /// untagged. Images decode single-threaded whatever `thread_count` says.
    [[nodiscard]] FramesResult roundtrip(const std::vector<Frame>& frames,
                                         const EncodeVideoOptions& encode_spec,
                                         const DecodeSpec& decode_spec = {});

    /// Single-image equivalent.
    [[nodiscard]] FrameResult roundtrip(const Frame& frame,
                                        const EncodeImageOptions& encode_spec,
                                        const DecodeSpec& decode_spec = {});

    /// The best attempt of a search: its bytes, its record, whose evidence
    /// holds the search, and its configuration, which holds the quality the
    /// search settled on.
    struct EncodeToTargetResult
    {
        std::vector<std::uint8_t> bytes;
        StageRecord record;
        StageConfiguration configuration;

        /// The search: every attempt, the quality settled on, what it
        /// achieved, and whether it converged.
        [[nodiscard]] const EncodeSearch& search() const;
    };

    /// Searches a codec's quality parameter to hit a target.
    ///
    /// Codec parameters are not comparable to each other: CRF 23 in x264 is not
    /// CRF 23 in x265, and neither means anything to WebP. Targeting bits per
    /// pixel or a quality metric instead gives one unit that severity can be
    /// sampled in across every codec, which is what makes two classes of data
    /// comparable on compression strength.
    [[nodiscard]] EncodeToTargetResult encode_to_target(const std::vector<Frame>& frames,
                                                        const EncodeVideoOptions& options,
                                                        const EncodeTarget& target);

    [[nodiscard]] EncodeToTargetResult encode_to_target(const Frame& frame,
                                                        const EncodeImageOptions& options,
                                                        const EncodeTarget& target);
}
