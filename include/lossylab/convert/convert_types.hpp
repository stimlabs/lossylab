#pragma once

#include "lossylab/core/codec_id.hpp"
#include "lossylab/core/color_spec.hpp"
#include "lossylab/core/kernel.hpp"
#include "lossylab/core/pixel_format.hpp"
#include "lossylab/core/reflect.hpp"
#include "lossylab/core/strict.hpp"

#include <optional>
#include <string>

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

    /// Options for relabeling a frame's color.
    struct ReinterpretOptions
    {
        /// The color the samples are declared to be in from now on.
        ColorSpec as_color;
    };

    LOSSYLAB_REFLECT(ReinterpretOptions, as_color);

    /// What a conversion decided for one frame.
    struct ConvertEvidence
    {
        /// The kernel the chroma planes were resampled with: "chroma_down"
        /// when they shrank, "chroma_up" otherwise.
        std::string kernel_role;
    };

    LOSSYLAB_REFLECT(ConvertEvidence, kernel_role);

    /// What a chroma round trip decided for one frame.
    struct ChromaRoundtripEvidence
    {
        /// The subsampled YUV format the frame passed through.
        PixelFormat intermediate_pixel_format;
    };

    LOSSYLAB_REFLECT(ChromaRoundtripEvidence, intermediate_pixel_format);

    /// A relabeling observes nothing about the frame.
    struct ReinterpretEvidence
    {
    };

    LOSSYLAB_REFLECT_EMPTY(ReinterpretEvidence);
}
