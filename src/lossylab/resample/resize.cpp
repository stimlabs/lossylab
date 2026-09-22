#include "lossylab/resample/resize.hpp"

#include "lossylab/core/error.hpp"
#include "lossylab/env/capabilities.hpp"

#include <algorithm>
#include <cmath>

namespace lossylab
{
    TargetSize TargetSize::absolute(const int width, const int height) noexcept
    {
        TargetSize size;
        size.m_mode = Mode::Absolute;
        size.m_width = width;
        size.m_height = height;
        return size;
    }

    TargetSize TargetSize::scale(const double factor) noexcept
    {
        return scale(factor, factor);
    }

    TargetSize TargetSize::scale(const double x_factor, const double y_factor) noexcept
    {
        TargetSize size;
        size.m_mode = Mode::Scale;
        size.m_x_factor = x_factor;
        size.m_y_factor = y_factor;
        return size;
    }

    TargetSize TargetSize::longest_side(const int length) noexcept
    {
        TargetSize size;
        size.m_mode = Mode::LongestSide;
        size.m_width = length;
        return size;
    }

    std::pair<int, int> TargetSize::resolve(const int input_width, const int input_height) const
    {
        if (input_width <= 0 || input_height <= 0)
        {
            throw ConfigError("cannot resolve a target size against a " +
                              std::to_string(input_width) + "x" +
                              std::to_string(input_height) + " input");
        }

        int output_width = 0;
        int output_height = 0;

        switch (m_mode)
        {
        case Mode::Absolute:
            output_width = m_width;
            output_height = m_height;
            break;

        case Mode::Scale:
            output_width = static_cast<int>(std::lround(input_width * m_x_factor));
            output_height = static_cast<int>(std::lround(input_height * m_y_factor));
            break;

        case Mode::LongestSide:
        {
            const double factor =
                static_cast<double>(m_width) / std::max(input_width, input_height);
            output_width = static_cast<int>(std::lround(input_width * factor));
            output_height = static_cast<int>(std::lround(input_height * factor));
            break;
        }
        }

        // Rounding can reach zero on an extreme downscale; one pixel is the
        // floor rather than an error, since a zero-sized frame is unusable.
        output_width = std::max(1, output_width);
        output_height = std::max(1, output_height);

        return {output_width, output_height};
    }

    json::Value TargetSize::to_json() const
    {
        switch (m_mode)
        {
        case Mode::Absolute:
            return json::object({{"mode", "absolute"}, {"width", m_width}, {"height", m_height}});
        case Mode::Scale:
            return json::object(
                {{"mode", "scale"}, {"x_factor", m_x_factor}, {"y_factor", m_y_factor}});
        case Mode::LongestSide:
            return json::object({{"mode", "longest_side"}, {"length", m_width}});
        }
        return json::Value();
    }

    TargetSize TargetSize::from_json(const json::Value& value)
    {
        const std::string& mode_name = value.at("mode").get_ref<const std::string&>();
        if (mode_name == "absolute")
        {
            return absolute(static_cast<int>(value.at("width").get<std::int64_t>()),
                            static_cast<int>(value.at("height").get<std::int64_t>()));
        }
        if (mode_name == "scale")
        {
            return scale(value.at("x_factor").get<double>(), value.at("y_factor").get<double>());
        }
        if (mode_name == "longest_side")
        {
            return longest_side(static_cast<int>(value.at("length").get<std::int64_t>()));
        }
        throw ConfigError("unknown target size mode '" + mode_name + "'");
    }

    ResizeResult resize(const Frame& frame, const ResizeOptions& options)
    {
        if (frame.empty())
        {
            throw ConfigError("resize() received an empty frame");
        }
        frame.color().require_fully_specified("resize()");

        // Resolved up front so an impossible request fails before any work, and
        // so the record can carry the achieved geometry rather than the asked-for
        // factor.
        const auto [width, height] = options.size.resolve(frame.width(), frame.height());
        static_cast<void>(width);
        static_cast<void>(height);

        capabilities().require_resize_backend(options.backend);

        if (options.mask.has_value() && options.mask->empty())
        {
            throw ConfigError("resize() was given an empty mask");
        }

        LL_NOT_IMPLEMENTED();
    }

    ResizeResult resize(const Frame& frame, const int width, const int height,
                        const Kernel kernel)
    {
        ResizeOptions options;
        options.size = TargetSize::absolute(width, height);
        options.kernel = KernelSpec{kernel, {}};
        return resize(frame, options);
    }
}
