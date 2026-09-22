#pragma once

#include "lossylab/core/codec_id.hpp"
#include "lossylab/core/frame.hpp"
#include "lossylab/core/geometry.hpp"
#include "lossylab/core/kernel.hpp"
#include "lossylab/core/result.hpp"
#include "lossylab/core/strict.hpp"

#include <optional>
#include <variant>

namespace lossylab
{
    /// Target size, either absolute or as a scale factor.
    struct TargetSize
    {
        static TargetSize absolute(int width, int height) noexcept;
        static TargetSize scale(double factor) noexcept;
        static TargetSize scale(double x_factor, double y_factor) noexcept;

        /// Scales the longest side to `length`, keeping the aspect ratio. The
        /// common "fit within a box" case that platform pipelines apply.
        static TargetSize longest_side(int length) noexcept;

        /// Resolves against an input size. Rounds to whole pixels, so the
        /// achieved scale may differ slightly from the requested one; the
        /// record carries the achieved transform, not the requested factor.
        [[nodiscard]] std::pair<int, int> resolve(int input_width, int input_height) const;

        [[nodiscard]] json::Value to_json() const;
        static TargetSize from_json(const json::Value& value);

    private:
        enum class Mode
        {
            Absolute,
            Scale,
            LongestSide
        };

        Mode m_mode = Mode::Absolute;
        int m_width = 0;
        int m_height = 0;
        double m_x_factor = 1.0;
        double m_y_factor = 1.0;
    };

    struct ResizeOptions
    {
        TargetSize size = TargetSize::scale(1.0);

        KernelSpec kernel{Kernel::Bicubic, {}};

        ResizeBackend backend = ResizeBackend::Swscale;

        /// Resizing cannot change color, so any color change here would be one
        /// the backend inserted. Refusing is the default for that reason.
        Strict strict = Strict::Refuse;

        /// A mask resampled under identical geometry with nearest-neighbor.
        ///
        /// Interpolating a mask would invent label values that were never in
        /// it, so it gets nearest-neighbor regardless of the image kernel.
        /// Resampling it here rather than separately is what guarantees the
        /// two stay aligned: the same transform is applied to both.
        std::optional<Frame> mask;
    };

    /// The result of a resize, with the mask alongside it when one was given.
    struct ResizeResult
    {
        Frame frame;
        std::optional<Frame> mask;
        StageRecord record;
    };

    /// Resamples a frame.
    ///
    /// Kernel diversity here is the point: real pipelines downscale with
    /// everything from a box filter to Lanczos, and upscalers are a strong
    /// source of hard negatives. The record holds the exact coordinate
    /// transform, so a crop in the output traces back to source pixels.
    [[nodiscard]] ResizeResult resize(const Frame& frame, const ResizeOptions& options);

    /// Convenience overload for an absolute target size.
    [[nodiscard]] ResizeResult resize(const Frame& frame, int width, int height,
                                      Kernel kernel = Kernel::Bicubic);
}
