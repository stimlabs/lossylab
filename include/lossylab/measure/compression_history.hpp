#pragma once

#include "lossylab/core/codec_id.hpp"
#include "lossylab/core/frame.hpp"
#include "lossylab/core/json.hpp"
#include "lossylab/core/pixel_format.hpp"
#include "lossylab/core/record.hpp"
#include "lossylab/core/reflect.hpp"
#include "lossylab/io/decode_image.hpp"
#include "lossylab/measure/compression_history_types.hpp"
#include "lossylab/measure/measure.hpp"

#include <optional>
#include <variant>

namespace lossylab
{
    struct CompressionHistory
    {
        /// Its evidence holds the traces found (see `evidence()`).
        StageRecord record;

        /// What the analysis was told to do (see `FrameResult::configuration`).
        CompressionHistoryOptions configuration;

        /// The traces, and the JPEG tables, chroma evidence and recompression
        /// curves they rest on.
        [[nodiscard]] const CompressionHistoryEvidence& evidence() const
        {
            return std::get<CompressionHistoryEvidence>(record.evidence);
        }

        [[nodiscard]] json::Value to_json() const;
    };

    /// Looks for every trace of earlier lossy compression in a decoded still
    /// image: which codec, roughly what quality, and which chroma
    /// subsampling.
    ///
    /// Takes any pixel format. What is analyzed is recorded, never guessed
    /// by the caller: a frame is read as JPEG's YCbCr (BT.601, full range)
    /// directly when it is in 8-bit planar YUV of that color or 8-bit gray,
    /// and converted to rgb24 or gray first otherwise, alpha dropped, with
    /// the conversion recorded. Three kinds of evidence are gathered:
    ///
    /// - JPEG quantization, read directly from the 8x8 DCT coefficients at
    ///   every grid offset, which gives the luma and chroma tables, the
    ///   libjpeg quality they correspond to, and the chroma layout.
    /// - Chroma subsampling, from whether the chroma is exactly an upsampled
    ///   plane (see ChromaSubsamplingEvidence).
    /// - Recompression curves for the codecs in the options, measured on
    ///   luma, in the chroma layout found above for MJPEG.
    ///
    /// The evidence also lists the analyzed format, the recompression crop,
    /// and any codec skipped or failed.
    [[nodiscard]] CompressionHistory compression_history(const Frame& frame,
                                                         const CompressionHistoryOptions& options = {});

    /// As above, for a decoded image. When it is a JPEG file decoded as
    /// stored (no orientation applied, no conversion, no tile grid, the
    /// frame untouched since), in 8 bits, gray or YCbCr, the JPEG tables and
    /// chroma layout come from the file's header, with a JpegHeader trace,
    /// and the pixels are not searched for a lattice unless the options ask
    /// for `jpeg_pixel_check`. Otherwise the frame is analyzed as above, and
    /// the evidence says why the header was not used.
    [[nodiscard]] CompressionHistory compression_history(const DecodedImage& image,
                                                         const CompressionHistoryOptions& options = {});

    namespace detail
    {
        struct LatticeSums
        {
            double squared_distance = 0.0;
            std::size_t unit_count = 0;
        };

        /// Over `count` non-negative magnitudes, the sum of their squared
        /// distances from the lattice of steps 1 / `inverse_step`, in steps,
        /// and how many lie nearest its first nonzero point. The magnitudes
        /// are summed in four lanes over whole groups of four, as
        /// lattice_sums_avx2() sums them, so that both agree to the bit.
        [[nodiscard]] LatticeSums lattice_sums_scalar(const double* magnitudes, std::size_t count,
                                                      double inverse_step);

        /// lattice_sums_scalar() with AVX2; absent when this build or
        /// processor has none.
        [[nodiscard]] std::optional<LatticeSums> lattice_sums_avx2(const double* magnitudes, std::size_t count,
                                                                   double inverse_step);
    }
}
