#pragma once

#include "lossylab/core/frame.hpp"
#include "lossylab/core/geometry.hpp"
#include "lossylab/core/kernel.hpp"
#include "lossylab/core/rational.hpp"
#include "lossylab/core/result.hpp"

#include <cstdint>
#include <optional>
#include <vector>

namespace lossylab
{
    /// The camera path a still is animated along.
    struct Trajectory
    {
        /// Pixels per second, in the source image's coordinates.
        double pan_x = 0.0;
        double pan_y = 0.0;

        /// Scale factor per second. 1.0 holds, above 1.0 zooms in.
        double zoom_rate = 1.0;

        /// Degrees per second.
        double rotation_rate = 0.0;

        /// Optional starting offsets, so a clip need not begin centered.
        double start_x = 0.0;
        double start_y = 0.0;
        double start_zoom = 1.0;
        double start_rotation = 0.0;

        /// Eases the motion in and out instead of moving linearly. Constant
        /// velocity is rare in real footage, and a detector can learn it.
        bool ease = false;

        /// The transform from still coordinates to frame `index`.
        [[nodiscard]] CoordinateTransform at(int index, int frame_count, Rational fps) const;

        [[nodiscard]] json::Value to_json() const;
        static Trajectory from_json(const json::Value& value);
    };

    /// Per-frame noise, applied independently to each frame.
    ///
    /// Independence is the point. A still repeated verbatim compresses almost
    /// to nothing after the first frame, which is nothing like real footage;
    /// independent noise gives the encoder something to spend bits on, and the
    /// resulting P and B frames carry realistic artifacts.
    struct TemporalNoise
    {
        /// Gaussian sigma in 8-bit sample units. Zero disables it.
        double sigma = 0.0;

        /// Correlation between consecutive frames, 0 for fully independent and
        /// 1 for static. Real sensor noise is close to independent; a light
        /// correlation models temporal denoising applied before encoding.
        double temporal_correlation = 0.0;

        [[nodiscard]] json::Value to_json() const;
        static TemporalNoise from_json(const json::Value& value);
    };

    struct AnimateStillOptions
    {
        int frame_count = 25;
        Rational fps{25, 1};

        Trajectory trajectory;
        TemporalNoise temporal_noise;

        /// Kernel used to resample the still into each frame.
        KernelSpec kernel{Kernel::Bicubic, {}};

        /// Output size. Defaults to the source size.
        std::optional<std::pair<int, int>> output_size;

        /// Required whenever temporal noise is enabled, so the sequence can be
        /// reproduced exactly from the record.
        std::uint64_t seed = 0;

        /// A mask carried along the same path, resampled with nearest
        /// neighbor. What makes an inpainting mask traceable through the
        /// animation.
        std::optional<Frame> mask;
    };

    struct AnimateStillResult
    {
        std::vector<Frame> frames;
        std::vector<Frame> masks;

        /// One record for the animation, whose per-frame entries carry the
        /// transform back to the still. Inverting frame `i`'s transform maps a
        /// crop in that frame to the pixels of the original image.
        StageRecord record;

        /// Per-frame transforms from the still to each output frame.
        std::vector<CoordinateTransform> transforms;
    };

    /// Builds a frame sequence from a single still image.
    ///
    /// Still-image sources have no P or B frames, so a video codec applied to
    /// them produces artifacts unlike anything in real footage. Giving the
    /// still a camera path and independent per-frame noise produces a sequence
    /// that compresses the way real video does, which is what makes video
    /// compression a usable augmentation for image datasets.
    [[nodiscard]] AnimateStillResult animate_still(const Frame& frame,
                                                   const AnimateStillOptions& options);
}
