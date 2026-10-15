#pragma once

/// How the library's encode options become one FFmpeg encoder's settings.
/// Internal header.

#include "encoder_session.hpp"

#include "lossylab/codec/encode.hpp"
#include "lossylab/core/geometry.hpp"

#include <optional>
#include <string>

namespace lossylab::detail
{
    /// The quality parameter's range for an image encode, or empty when the
    /// codec and `lossless` leave it none.
    [[nodiscard]] std::optional<QualityRange> image_quality_range(ImageCodec codec, bool lossless,
                                                                  const std::string& encoder_name);

    /// The quality parameter's range for a video encoder in a rate control
    /// mode, or empty when the mode has none. `bit_depth` is that of the
    /// pixel format the encoder receives.
    [[nodiscard]] std::optional<QualityRange> video_quality_range(const std::string& encoder_name,
                                                                  RateControl::Mode mode, int bit_depth);

    /// One encode, translated for a specific encoder.
    struct EncoderPlan
    {
        EncoderSetup setup;

        /// The range of the quality parameter the encode sets. Empty when the
        /// encode has no quality parameter.
        std::optional<QualityRange> quality_range;

        /// Where the encoder's blocks fall, for encoders whose grid is fixed.
        std::optional<BlockGrid> block_grid;

        /// Whether the encoder's per-packet quality statistics hold its
        /// quantizer.
        bool reports_qp = false;

        /// The muxer that makes the packets a file, or empty when an image
        /// encoder's single packet is the file already.
        std::string muxer;

        /// The file extension of the output, for decoding it back.
        std::string extension;
        /// Whether the output can carry the chroma location, so that reading
        /// it back tells whether the encoder kept it.
        bool signals_chroma_location = false;
    };

    /// Validates `options` against what the encoder and its format can carry
    /// and translates them. `pixel_format` and `color` are those of the frame
    /// the encoder receives, and `has_icc_profile` whether it carries one.
    [[nodiscard]] EncoderPlan plan_image_encode(const std::string& encoder_name, const EncodeImageOptions& options,
                                                const PixelFormat& pixel_format, const ColorSpec& color,
                                                bool has_icc_profile);

    /// As above for video. The container is the one the options name, or the
    /// codec's elementary stream format when they name none.
    [[nodiscard]] EncoderPlan plan_video_encode(const std::string& encoder_name, const EncodeVideoOptions& options,
                                                const PixelFormat& pixel_format);
}
