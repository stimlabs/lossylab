#include "lossylab/core/geometry.hpp"

#include "lossylab/core/error.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <string>
#include <tuple>
#include <utility>

namespace lossylab
{
    namespace
    {
        /// Tolerance for treating a coordinate as a whole pixel. Transforms are
        /// built from ratios of small integers, so accumulated error stays far
        /// below this even after a long pipeline.
        constexpr double epsilon = 1e-9;

        bool near(const double left, const double right) noexcept
        {
            return std::abs(left - right) < epsilon;
        }

        bool is_whole(const double value) noexcept
        {
            return near(value, std::round(value));
        }
    }

    json::Value Rect::to_json() const
    {
        return json::object({{"x", x}, {"y", y}, {"width", width}, {"height", height}});
    }

    Rect Rect::from_json(const json::Value& value)
    {
        return {
            value.at("x").get<double>(),
            value.at("y").get<double>(),
            value.at("width").get<double>(),
            value.at("height").get<double>(),
        };
    }

    bool operator==(const Rect& left, const Rect& right) noexcept
    {
        return left.x == right.x && left.y == right.y && left.width == right.width &&
               left.height == right.height;
    }

    CoordinateTransform::CoordinateTransform(const double initial_scale_x,
                                             const double initial_shear_x,
                                             const double initial_translate_x,
                                             const double initial_shear_y,
                                             const double initial_scale_y,
                                             const double initial_translate_y) noexcept
        : scale_x(initial_scale_x),
          shear_x(initial_shear_x),
          translate_x(initial_translate_x),
          shear_y(initial_shear_y),
          scale_y(initial_scale_y),
          translate_y(initial_translate_y)
    {
    }

    CoordinateTransform CoordinateTransform::identity() noexcept
    {
        return {};
    }

    CoordinateTransform CoordinateTransform::scaling(const double scale_x, const double scale_y) noexcept
    {
        return {scale_x, 0.0, 0.0, 0.0, scale_y, 0.0};
    }

    CoordinateTransform CoordinateTransform::translation(const double translate_x, const double translate_y) noexcept
    {
        return {1.0, 0.0, translate_x, 0.0, 1.0, translate_y};
    }

    CoordinateTransform CoordinateTransform::rotation_degrees(const double degrees) noexcept
    {
        const double radians = degrees * std::numbers::pi / 180.0;
        const double cosine = std::cos(radians);
        const double sine = std::sin(radians);
        return {cosine, -sine, 0.0, sine, cosine, 0.0};
    }

    CoordinateTransform CoordinateTransform::resize(const int input_width, const int input_height,
                                                    const int output_width,
                                                    const int output_height) noexcept
    {
        const double scale_x = static_cast<double>(output_width) / static_cast<double>(input_width);
        const double scale_y = static_cast<double>(output_height) / static_cast<double>(input_height);

        // Pixel centers are at i + 0.5 in both spaces, so a center at 0.5 in the
        // input must land at 0.5 * scale in the output: out = scale * (in + 0.5) - 0.5.
        return {scale_x, 0.0, 0.5 * scale_x - 0.5,
                0.0, scale_y, 0.5 * scale_y - 0.5};
    }

    CoordinateTransform CoordinateTransform::crop(const double x, const double y) noexcept
    {
        return translation(-x, -y);
    }

    CoordinateTransform CoordinateTransform::orientation(const int exif_orientation, const int width,
                                                         const int height)
    {
        const double last_column = width - 1;
        const double last_row = height - 1;
        switch (exif_orientation)
        {
        case 1: return identity();
        case 2: return {-1.0, 0.0, last_column, 0.0, 1.0, 0.0};
        case 3: return {-1.0, 0.0, last_column, 0.0, -1.0, last_row};
        case 4: return {1.0, 0.0, 0.0, 0.0, -1.0, last_row};
        case 5: return {0.0, 1.0, 0.0, 1.0, 0.0, 0.0};
        case 6: return {0.0, -1.0, last_row, 1.0, 0.0, 0.0};
        case 7: return {0.0, -1.0, last_row, -1.0, 0.0, last_column};
        case 8: return {0.0, 1.0, 0.0, -1.0, 0.0, last_column};
        default: throw ConfigError("EXIF orientation " + std::to_string(exif_orientation) + " is not in 1-8");
        }
    }

    CoordinateTransform CoordinateTransform::then(const CoordinateTransform& after) const noexcept
    {
        return {
            after.scale_x * scale_x + after.shear_x * shear_y,
            after.scale_x * shear_x + after.shear_x * scale_y,
            after.scale_x * translate_x + after.shear_x * translate_y + after.translate_x,
            after.shear_y * scale_x + after.scale_y * shear_y,
            after.shear_y * shear_x + after.scale_y * scale_y,
            after.shear_y * translate_x + after.scale_y * translate_y + after.translate_y,
        };
    }

    Point CoordinateTransform::map_forward(const Point point) const noexcept
    {
        return {scale_x * point.x + shear_x * point.y + translate_x,
                shear_y * point.x + scale_y * point.y + translate_y};
    }

    Rect CoordinateTransform::map_bounds(const Rect& rect) const noexcept
    {
        const Point corners[4] = {
            map_forward({rect.x, rect.y}),
            map_forward({rect.x + rect.width, rect.y}),
            map_forward({rect.x, rect.y + rect.height}),
            map_forward({rect.x + rect.width, rect.y + rect.height}),
        };

        double min_x = corners[0].x;
        double max_x = corners[0].x;
        double min_y = corners[0].y;
        double max_y = corners[0].y;
        for (const Point& corner : corners)
        {
            min_x = std::min(min_x, corner.x);
            max_x = std::max(max_x, corner.x);
            min_y = std::min(min_y, corner.y);
            max_y = std::max(max_y, corner.y);
        }
        return {min_x, min_y, max_x - min_x, max_y - min_y};
    }

    double CoordinateTransform::determinant() const noexcept
    {
        return scale_x * scale_y - shear_x * shear_y;
    }

    bool CoordinateTransform::is_invertible() const noexcept
    {
        return std::abs(determinant()) > epsilon;
    }

    CoordinateTransform CoordinateTransform::inverse() const
    {
        const double determinant_value = determinant();
        if (std::abs(determinant_value) <= epsilon)
        {
            throw ConfigError("coordinate transform is singular and cannot be inverted");
        }
        const double inverse_scale_x = scale_y / determinant_value;
        const double inverse_shear_x = -shear_x / determinant_value;
        const double inverse_shear_y = -shear_y / determinant_value;
        const double inverse_scale_y = scale_x / determinant_value;
        return {
            inverse_scale_x,
            inverse_shear_x,
            -(inverse_scale_x * translate_x + inverse_shear_x * translate_y),
            inverse_shear_y,
            inverse_scale_y,
            -(inverse_shear_y * translate_x + inverse_scale_y * translate_y),
        };
    }

    Point CoordinateTransform::map_inverse(const Point point) const
    {
        return inverse().map_forward(point);
    }

    bool CoordinateTransform::is_integer_translation() const noexcept
    {
        return near(scale_x, 1.0) && near(scale_y, 1.0) && near(shear_x, 0.0) && near(shear_y, 0.0) &&
               is_whole(translate_x) && is_whole(translate_y);
    }

    bool CoordinateTransform::is_identity() const noexcept
    {
        return near(scale_x, 1.0) && near(scale_y, 1.0) && near(shear_x, 0.0) && near(shear_y, 0.0) &&
               near(translate_x, 0.0) && near(translate_y, 0.0);
    }

    json::Value CoordinateTransform::to_json() const
    {
        return json::object({
            {"scale_x", scale_x}, {"shear_x", shear_x}, {"translate_x", translate_x},
            {"shear_y", shear_y}, {"scale_y", scale_y}, {"translate_y", translate_y},
        });
    }

    CoordinateTransform CoordinateTransform::from_json(const json::Value& value)
    {
        return {
            value.at("scale_x").get<double>(),
            value.at("shear_x").get<double>(),
            value.at("translate_x").get<double>(),
            value.at("shear_y").get<double>(),
            value.at("scale_y").get<double>(),
            value.at("translate_y").get<double>(),
        };
    }

    bool operator==(const CoordinateTransform& left, const CoordinateTransform& right) noexcept
    {
        return near(left.scale_x, right.scale_x) && near(left.shear_x, right.shear_x) &&
               near(left.translate_x, right.translate_x) && near(left.shear_y, right.shear_y) &&
               near(left.scale_y, right.scale_y) && near(left.translate_y, right.translate_y);
    }

    // -----------------------------------------------------------------------
    // BlockGrid
    // -----------------------------------------------------------------------

    std::string to_string(const BlockGridKind kind)
    {
        switch (kind)
        {
        case BlockGridKind::Dct8: return "dct8";
        case BlockGridKind::Macroblock16: return "macroblock16";
        case BlockGridKind::Ctu32: return "ctu32";
        case BlockGridKind::Ctu64: return "ctu64";
        case BlockGridKind::JpegMcu: return "jpeg_mcu";
        }
        return "unknown";
    }

    std::optional<BlockGridKind> block_grid_kind_from_string(const std::string_view name)
    {
        if (name == "dct8") { return BlockGridKind::Dct8; }
        if (name == "macroblock16") { return BlockGridKind::Macroblock16; }
        if (name == "ctu32") { return BlockGridKind::Ctu32; }
        if (name == "ctu64") { return BlockGridKind::Ctu64; }
        if (name == "jpeg_mcu") { return BlockGridKind::JpegMcu; }
        return std::nullopt;
    }

    BlockGrid BlockGrid::for_kind(const BlockGridKind kind) noexcept
    {
        BlockGrid grid;
        grid.kind = kind;
        switch (kind)
        {
        case BlockGridKind::Dct8: grid.block_width = grid.block_height = 8; break;
        case BlockGridKind::Macroblock16: grid.block_width = grid.block_height = 16; break;
        case BlockGridKind::Ctu32: grid.block_width = grid.block_height = 32; break;
        case BlockGridKind::Ctu64: grid.block_width = grid.block_height = 64; break;
        case BlockGridKind::JpegMcu: grid.block_width = grid.block_height = 8; break;
        }
        return grid;
    }

    BlockGrid BlockGrid::apply_transform(const CoordinateTransform& transform) const noexcept
    {
        BlockGrid result = *this;
        if (!valid)
        {
            return result;
        }
        const auto is_unit_or_zero = [](const double value)
        { return near(value, 0.0) || near(value, 1.0) || near(value, -1.0); };
        const bool permutes_axes = is_unit_or_zero(transform.scale_x) && is_unit_or_zero(transform.shear_x) &&
                                   is_unit_or_zero(transform.shear_y) && is_unit_or_zero(transform.scale_y) &&
                                   near(std::abs(transform.scale_x) + std::abs(transform.shear_x), 1.0) &&
                                   near(std::abs(transform.shear_y) + std::abs(transform.scale_y), 1.0) &&
                                   near(std::abs(transform.determinant()), 1.0);
        if (!permutes_axes || !is_whole(transform.translate_x) || !is_whole(transform.translate_y))
        {
            // Resampling, rotation by other than quarter turns, or a sub-pixel
            // shift: block boundaries no longer align to anything in the new
            // sampling grid.
            result.valid = false;
            return result;
        }

        // Each output axis is one input axis, possibly reversed, shifted by
        // whole pixels. A reversed axis puts a block's last pixel first, so
        // its new start is where the old end lands.
        const auto output_axis = [this](const double from_x, const double from_y, const double translate)
        {
            const bool from_input_x = !near(from_x, 0.0);
            const double sign = from_input_x ? from_x : from_y;
            const int size = from_input_x ? block_width : block_height;
            const int phase = from_input_x ? phase_x : phase_y;
            const auto shift = static_cast<int>(std::llround(translate));
            const int start = sign > 0.0 ? phase + shift : shift - phase - size + 1;

            // Euclidean modulo: the phase stays in [0, size) for negative shifts too.
            return std::pair<int, int>{size, (start % size + size) % size};
        };
        std::tie(result.block_width, result.phase_x) =
            output_axis(transform.scale_x, transform.shear_x, transform.translate_x);
        std::tie(result.block_height, result.phase_y) =
            output_axis(transform.shear_y, transform.scale_y, transform.translate_y);
        return result;
    }

    bool BlockGrid::is_block_origin(const int x, const int y) const noexcept
    {
        if (!valid)
        {
            return false;
        }
        const int local_x = ((x - phase_x) % block_width + block_width) % block_width;
        const int local_y = ((y - phase_y) % block_height + block_height) % block_height;
        return local_x == 0 && local_y == 0;
    }

    json::Value BlockGrid::to_json() const
    {
        return json::object({
            {"kind", to_string(kind)},
            {"block_width", block_width},
            {"block_height", block_height},
            {"phase_x", phase_x},
            {"phase_y", phase_y},
            {"valid", valid},
        });
    }

    BlockGrid BlockGrid::from_json(const json::Value& value)
    {
        const std::string& kind_name = value.at("kind").get_ref<const std::string&>();
        const auto kind = block_grid_kind_from_string(kind_name);
        if (!kind)
        {
            throw ConfigError("unknown block grid kind '" + kind_name + "'");
        }

        BlockGrid grid;
        grid.kind = *kind;
        grid.block_width = static_cast<int>(value.at("block_width").get<std::int64_t>());
        grid.block_height = static_cast<int>(value.at("block_height").get<std::int64_t>());
        grid.phase_x = static_cast<int>(value.at("phase_x").get<std::int64_t>());
        grid.phase_y = static_cast<int>(value.at("phase_y").get<std::int64_t>());
        grid.valid = value.at("valid").get<bool>();
        return grid;
    }

    bool operator==(const BlockGrid& left, const BlockGrid& right) noexcept
    {
        return left.kind == right.kind && left.block_width == right.block_width &&
               left.block_height == right.block_height && left.phase_x == right.phase_x &&
               left.phase_y == right.phase_y && left.valid == right.valid;
    }
}
