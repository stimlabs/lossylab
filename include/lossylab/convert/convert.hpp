#pragma once

#include "lossylab/convert/convert_types.hpp"
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
    /// Converts pixel format, bit depth, subsampling and color in one step.
    ///
    /// The building block of every codec path. Spatial dimensions are not
    /// changed; use `resize` for that. The returned record names the source and
    /// target formats, the kernels actually used, and every property that
    /// changed, so two datasets can be compared on their conversion history
    /// rather than on trust.
    ///
    /// swscale runs with accurate rounding, bit-exact arithmetic and full
    /// chroma interpolation, so the chroma kernel asked for is the one used at
    /// every frame size, and with one thread.
    ///
    /// A target of rgb24 takes one path for every source: swscale to 16-bit
    /// RGB, then the color conversion to sRGB (`icc`), then compositing over
    /// black (`alpha`), then a single exact rounding to 8 bits. No dither is
    /// applied, and an 8-bit RGB, palette or gray source keeps its values.
    ///
    /// Otherwise the target must keep the source's primaries and transfer:
    /// swscale applies the YUV matrix and range only, so a change of gamut or
    /// tone curve throws NotImplemented rather than being labeled as done.
    /// `reinterpret` relabels those fields when that is what is meant.
    [[nodiscard]] FrameResult convert(const Frame& frame, const ConvertOptions& options);

    /// Convenience overload: a target format and color with the default
    /// kernels. It repackages the samples only: no ICC profile is applied
    /// (IccHandling::Ignore) and an alpha channel the target lacks is dropped
    /// (AlphaHandling::Discard).
    [[nodiscard]] FrameResult convert(const Frame& frame, PixelFormat pixel_format,
                                      const ColorSpec& color,
                                      Strict strict = Strict::AllowRecorded);

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
    [[nodiscard]] FrameResult reinterpret(const Frame& frame, const ReinterpretOptions& options);

    /// Convenience overload: relabels as `as_color`.
    [[nodiscard]] FrameResult reinterpret(const Frame& frame, const ColorSpec& as_color);
}
