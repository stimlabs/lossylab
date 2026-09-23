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
    /// The values an encoder's quality parameter takes.
    struct QualityRange
    {
        double minimum = 0.0;
        double maximum = 0.0;
        bool integral = true;
        bool higher_is_better = false;
    };

    /// One encode, translated for a specific encoder.
    struct EncoderPlan
    {
        EncoderSetup setup;

        /// What the quality parameter means for this encoder, e.g. "crf,
        /// integer 0 to 63, lower is better". Empty when the encode has no
        /// quality parameter.
        std::string quality_scale;
        std::optional<QualityRange> quality_range;

        /// Where the encoder's blocks fall, for encoders whose grid is fixed.
        std::optional<BlockGrid> block_grid;

        /// Whether the encoder's per-packet quality statistics hold its
        /// quantizer, and in which units.
        bool reports_qp = false;
        std::string qp_scale;

        /// The muxer that makes the packets a file, or empty when an image
        /// encoder's single packet is the file already.
        std::string muxer;

        /// The file extension of the output, for decoding it back.
        std::string extension;
    };

    /// Validates `options` against what the encoder and its format can carry
    /// and translates them. `pixel_format` and `color` are those of the frame
    /// the encoder receives.
    [[nodiscard]] EncoderPlan plan_image_encode(const std::string& encoder_name, const EncodeImageOptions& options,
                                                const PixelFormat& pixel_format, const ColorSpec& color);

    /// As above for video. The container is the one the options name, or the
    /// codec's elementary stream format when they name none.
    [[nodiscard]] EncoderPlan plan_video_encode(const std::string& encoder_name, const EncodeVideoOptions& options,
                                                const PixelFormat& pixel_format);
}
