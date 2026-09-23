#pragma once

#include "lossylab/core/codec_id.hpp"
#include "lossylab/core/color_spec.hpp"
#include "lossylab/core/frame.hpp"
#include "lossylab/core/kernel.hpp"
#include "lossylab/core/pixel_format.hpp"
#include "lossylab/core/reflect.hpp"
#include "lossylab/core/result.hpp"
#include "lossylab/core/strict.hpp"

namespace lossylab
{
    /// How a conversion should be carried out.
    ///
    /// Everything here is explicit on purpose. A conversion that picked its own
    /// chroma kernel, or its own idea of the source color space, would be a
    /// difference between two datasets that nobody chose and nobody can see.
    struct ConvertOptions
    {
        /// Target sample layout. Must be valid.
        PixelFormat pixel_format;

        /// Target color interpretation. Must be fully specified.
        ColorSpec color;

        /// Kernel used when chroma planes shrink, i.e. going to a coarser
        /// subsampling. This is the step that discards information, so it is
        /// the one whose kernel matters most.
        KernelSpec chroma_down{Kernel::Area, {}};

        /// Kernel used when chroma planes grow, i.e. going to a finer
        /// subsampling or back to RGB.
        KernelSpec chroma_up{Kernel::Bilinear, {}};

        ResizeBackend backend = ResizeBackend::Swscale;

        /// Defaults to AllowRecorded, unlike filter graphs and encoders.
        ///
        /// Nothing about a convert is hidden: the caller names the target
        /// format and the target color, so refusing by default would only mean
        /// refusing what was just asked for. Strict mode is where FFmpeg might
        /// insert a conversion on its own, which is filtering and encoding.
        ///
        /// Passing Refuse here is still useful, as an assertion: it means
        /// "change the sample layout but do not touch the color", and throws if
        /// the call would. The change an RGB/YUV switch entails is exempt, or
        /// the assertion could never hold across that boundary.
        Strict strict = Strict::AllowRecorded;
    };

    LOSSYLAB_REFLECT(ConvertOptions, pixel_format, color, chroma_down, chroma_up, backend, strict);

    /// Converts pixel format, bit depth, subsampling and color in one step.
    ///
    /// The building block of every codec path. Spatial dimensions are not
    /// changed; use `resize` for that. The returned record names the source and
    /// target formats, the kernels actually used, and every property that
    /// changed, so two datasets can be compared on their conversion history
    /// rather than on trust.
    ///
    /// The target must keep the source's primaries and transfer: swscale's
    /// path here applies the YUV matrix and range only, so a change of gamut
    /// or tone curve throws NotImplemented rather than being labeled as done.
    /// `reinterpret` relabels those fields when that is what is meant.
    [[nodiscard]] FrameResult convert(const Frame& frame, const ConvertOptions& options);

    /// Convenience overload for the common case: a target format and color with
    /// default kernels.
    [[nodiscard]] FrameResult convert(const Frame& frame, PixelFormat pixel_format,
                                      const ColorSpec& color,
                                      Strict strict = Strict::AllowRecorded);

    /// Options for a chroma subsampling round trip.
    struct ChromaRoundtripOptions
    {
        /// The subsampling to pass through. 4:2:0 for most delivery formats.
        Subsampling subsampling = Subsampling::Yuv420;

        /// The color interpretation used for the RGB->YUV->RGB journey. Must be
        /// fully specified: which matrix and range the trip runs through is
        /// itself part of the trace being applied. Its primaries and transfer
        /// are not used: the intermediate keeps the source's.
        ColorSpec color = ColorSpec::bt709_limited();

        KernelSpec chroma_down{Kernel::Area, {}};
        KernelSpec chroma_up{Kernel::Bilinear, {}};

        /// Bit depth of the intermediate YUV. Defaults to matching the source.
        std::optional<int> intermediate_bit_depth;

        ResizeBackend backend = ResizeBackend::Swscale;
        Strict strict = Strict::AllowRecorded;
    };

    LOSSYLAB_REFLECT(ChromaRoundtripOptions, subsampling, color, chroma_down, chroma_up, intermediate_bit_depth,
                      backend, strict);

    /// RGB to subsampled YUV and back, in one call.
    ///
    /// Serves two purposes from one implementation: as augmentation it applies
    /// a realistic chroma history, and as equalization it gives synthetic
    /// images the same chroma history that camera-captured ones already carry,
    /// so a detector cannot separate the classes on that alone.
    ///
    /// Note the default of Strict::AllowRecorded: the whole point of the call
    /// is to convert, so refusing would be perverse. Every step still lands in
    /// the record.
    [[nodiscard]] FrameResult chroma_roundtrip(const Frame& frame,
                                               const ChromaRoundtripOptions& options);

    /// Relabels a frame's color without touching a single sample.
    ///
    /// This is how BT.601/BT.709 and limited/full mix-ups are reproduced: the
    /// numbers stay exactly as they were, and only their declared meaning
    /// changes, which is precisely what happens when a real pipeline loses or
    /// misreads a tag. Because no sample changes, this is lossless and
    /// perfectly reversible.
    [[nodiscard]] FrameResult reinterpret(const Frame& frame, const ColorSpec& as_color);
}
