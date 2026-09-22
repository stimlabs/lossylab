#include "lossylab/motion/animate_still.hpp"

#include "lossylab/core/error.hpp"

#include <cmath>

namespace lossylab
{
    namespace
    {
        /// Smoothstep, so eased motion starts and ends at rest.
        double ease_fraction(const double fraction)
        {
            return fraction * fraction * (3.0 - 2.0 * fraction);
        }
    }

    CoordinateTransform Trajectory::at(const int index, const int frame_count,
                                       const Rational fps) const
    {
        if (frame_count <= 0)
        {
            throw ConfigError("trajectory needs a positive frame count");
        }
        if (index < 0 || index >= frame_count)
        {
            throw ConfigError("frame index " + std::to_string(index) +
                              " is outside a " + std::to_string(frame_count) +
                              "-frame trajectory");
        }
        if (!fps.is_valid() || fps.to_double() <= 0.0)
        {
            throw ConfigError("trajectory needs a positive frame rate");
        }

        // Progress is expressed as a fraction first so easing can be applied,
        // then converted to seconds: easing has to bend the path, not the clock.
        const double duration = (frame_count - 1) / fps.to_double();
        const double fraction =
            frame_count == 1 ? 0.0 : static_cast<double>(index) / (frame_count - 1);
        const double progress = ease ? ease_fraction(fraction) : fraction;
        const double seconds = progress * duration;

        const double zoom = start_zoom * std::pow(zoom_rate, seconds);
        if (zoom <= 0.0)
        {
            throw ConfigError("trajectory zoom reached zero or negative scale");
        }

        const double degrees = start_rotation + rotation_rate * seconds;
        const double offset_x = start_x + pan_x * seconds;
        const double offset_y = start_y + pan_y * seconds;

        return CoordinateTransform::translation(-offset_x, -offset_y)
            .then(CoordinateTransform::scaling(zoom, zoom))
            .then(CoordinateTransform::rotation_degrees(degrees));
    }

    json::Value Trajectory::to_json() const
    {
        return json::object({
            {"pan_x", pan_x},
            {"pan_y", pan_y},
            {"zoom_rate", zoom_rate},
            {"rotation_rate", rotation_rate},
            {"start_x", start_x},
            {"start_y", start_y},
            {"start_zoom", start_zoom},
            {"start_rotation", start_rotation},
            {"ease", ease},
        });
    }

    Trajectory Trajectory::from_json(const json::Value& value)
    {
        Trajectory trajectory;
        trajectory.pan_x = json::double_or(value, "pan_x", 0.0);
        trajectory.pan_y = json::double_or(value, "pan_y", 0.0);
        trajectory.zoom_rate = json::double_or(value, "zoom_rate", 1.0);
        trajectory.rotation_rate = json::double_or(value, "rotation_rate", 0.0);
        trajectory.start_x = json::double_or(value, "start_x", 0.0);
        trajectory.start_y = json::double_or(value, "start_y", 0.0);
        trajectory.start_zoom = json::double_or(value, "start_zoom", 1.0);
        trajectory.start_rotation = json::double_or(value, "start_rotation", 0.0);
        trajectory.ease = json::bool_or(value, "ease", false);
        return trajectory;
    }

    json::Value TemporalNoise::to_json() const
    {
        return json::object({
            {"sigma", sigma},
            {"temporal_correlation", temporal_correlation},
        });
    }

    TemporalNoise TemporalNoise::from_json(const json::Value& value)
    {
        TemporalNoise noise;
        noise.sigma = json::double_or(value, "sigma", 0.0);
        noise.temporal_correlation = json::double_or(value, "temporal_correlation", 0.0);
        return noise;
    }

    AnimateStillResult animate_still(const Frame& frame, const AnimateStillOptions& options)
    {
        if (frame.empty())
        {
            throw ConfigError("animate_still() received an empty frame");
        }
        if (options.frame_count < 1)
        {
            throw ConfigError("animate_still() needs at least one frame");
        }
        if (!options.fps.is_valid() || options.fps.to_double() <= 0.0)
        {
            throw ConfigError("animate_still() needs a positive frame rate");
        }
        if (options.temporal_noise.sigma < 0.0)
        {
            throw ConfigError("temporal noise sigma cannot be negative");
        }
        if (options.temporal_noise.temporal_correlation < 0.0 ||
            options.temporal_noise.temporal_correlation > 1.0)
        {
            throw ConfigError("temporal correlation must be between 0 and 1");
        }

        frame.color().require_fully_specified("animate_still()");

        // The trajectory is resolved for every frame before any pixels move, so
        // an impossible path fails immediately rather than midway through.
        for (int index = 0; index < options.frame_count; ++index)
        {
            static_cast<void>(
                options.trajectory.at(index, options.frame_count, options.fps));
        }

        LL_NOT_IMPLEMENTED();
    }
}
