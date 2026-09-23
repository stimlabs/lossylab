#pragma once

#include "lossylab/core/json.hpp"
#include "lossylab/core/reflect.hpp"

#include <optional>
#include <string>

namespace lossylab
{
    /// A position in pixel-index space: pixel `i` has coordinate exactly `i`,
    /// so (0, 0) is the center of the top-left pixel and the image occupies
    /// [-0.5, width - 0.5]. Masks and crops are expressed in pixel indices, so
    /// this is the convention that keeps them round-trippable without an
    /// off-by-half at every stage.
    struct Point
    {
        double x = 0.0;
        double y = 0.0;
    };

    LOSSYLAB_REFLECT(Point, x, y);

    struct Rect
    {
        double x = 0.0;
        double y = 0.0;
        double width = 0.0;
        double height = 0.0;

        [[nodiscard]] json::Value to_json() const;
        static Rect from_json(const json::Value& value);
    };

    LOSSYLAB_REFLECT(Rect, x, y, width, height);

    bool operator==(const Rect& left, const Rect& right) noexcept;
    inline bool operator!=(const Rect& left, const Rect& right) noexcept
    {
        return !(left == right);
    }

    /// An affine map from input pixel coordinates to output pixel coordinates,
    /// stored as the 2x3 matrix
    ///
    ///     | scale_x  shear_x  translate_x |
    ///     | shear_y  scale_y  translate_y |
    ///
    /// Every stage records one. Composing them across a pipeline is what lets
    /// an output crop, or an inpainting mask, be traced back to the source
    /// pixels it came from.
    class CoordinateTransform
    {
    public:
        CoordinateTransform() = default;
        CoordinateTransform(double scale_x, double shear_x, double translate_x,
                            double shear_y, double scale_y, double translate_y) noexcept;

        static CoordinateTransform identity() noexcept;
        static CoordinateTransform scaling(double scale_x, double scale_y) noexcept;
        static CoordinateTransform translation(double translate_x, double translate_y) noexcept;
        static CoordinateTransform rotation_degrees(double degrees) noexcept;

        /// Maps a resize of the input size onto the output size.
        ///
        /// Image *edges* align, not pixel index 0 with pixel index 0, so the
        /// map is `output = scale * input + (scale - 1) / 2`. Using a bare
        /// output/input ratio with no offset is the classic half-pixel-shift
        /// bug: it lines the two grids up at the top-left corner and drifts
        /// everywhere else.
        static CoordinateTransform resize(int input_width, int input_height,
                                          int output_width, int output_height) noexcept;

        /// Maps a crop whose top-left corner lies at (x, y) in the input.
        static CoordinateTransform crop(double x, double y) noexcept;

        /// Maps a `width` x `height` image onto the same image turned
        /// upright for an EXIF orientation (1-8): whole-pixel flips and
        /// quarter turns, so pixel centers land exactly on pixel centers.
        /// Throws ConfigError outside 1-8.
        static CoordinateTransform orientation(int exif_orientation, int width, int height);

        /// `after` applied to the result of `*this`, i.e. `after ∘ this`.
        [[nodiscard]] CoordinateTransform then(const CoordinateTransform& after) const noexcept;

        [[nodiscard]] Point map_forward(Point point) const noexcept;

        /// Throws ConfigError when the transform is singular.
        [[nodiscard]] Point map_inverse(Point point) const noexcept(false);

        /// The axis-aligned bounding box of the four mapped corners. Named
        /// distinctly from map_forward because `{x, y}` would otherwise be
        /// ambiguous between a Point and a Rect.
        [[nodiscard]] Rect map_bounds(const Rect& rect) const noexcept;

        [[nodiscard]] CoordinateTransform inverse() const noexcept(false);

        [[nodiscard]] double determinant() const noexcept;
        [[nodiscard]] bool is_invertible() const noexcept;

        /// True when the transform only shifts by whole pixels: unit scale, no
        /// shear, integer translation. This is the property that decides
        /// whether a block grid survives the stage.
        [[nodiscard]] bool is_integer_translation() const noexcept;

        [[nodiscard]] bool is_identity() const noexcept;

        [[nodiscard]] json::Value to_json() const;
        static CoordinateTransform from_json(const json::Value& value);

        double scale_x = 1.0;
        double shear_x = 0.0;
        double translate_x = 0.0;
        double shear_y = 0.0;
        double scale_y = 1.0;
        double translate_y = 0.0;
    };

    bool operator==(const CoordinateTransform& left, const CoordinateTransform& right) noexcept;
    inline bool operator!=(const CoordinateTransform& left, const CoordinateTransform& right) noexcept
    {
        return !(left == right);
    }

    /// The coding block grid a compression stage imposed, expressed in output
    /// coordinates.
    enum class BlockGridKind
    {
        Dct8,        ///< JPEG / MJPEG / WebP 8x8 DCT grid
        Macroblock16, ///< H.264 macroblocks
        Ctu32,       ///< HEVC / VP9 / AV1 coding tree units
        Ctu64
    };

    std::string to_string(BlockGridKind kind);
    std::optional<BlockGridKind> block_grid_kind_from_string(std::string_view name);

    /// Where a compression stage's block boundaries fall in the current frame.
    ///
    /// `valid` is the interesting field: a grid survives cropping and padding
    /// by whole pixels (its phase merely shifts) and quarter turns or flips
    /// (its axes swap or reverse), but is destroyed by resampling, any other
    /// rotation or any fractional shift. `apply_transform` enforces that rule
    /// centrally, so no individual stage has to remember it.
    struct BlockGrid
    {
        BlockGridKind kind = BlockGridKind::Dct8;
        int block_width = 8;
        int block_height = 8;
        int phase_x = 0;
        int phase_y = 0;
        bool valid = true;

        static BlockGrid for_kind(BlockGridKind kind) noexcept;

        /// Returns the grid as seen after `transform` is applied. An integer
        /// translation shifts the phase; a quarter turn or flip about whole
        /// pixels swaps or reverses the axes as well; anything else
        /// invalidates the grid.
        [[nodiscard]] BlockGrid apply_transform(const CoordinateTransform& transform) const noexcept;

        /// True when (x, y) in output coordinates is the top-left corner of a
        /// block. Meaningless, and always false, on an invalid grid.
        [[nodiscard]] bool is_block_origin(int x, int y) const noexcept;

        [[nodiscard]] json::Value to_json() const;
        static BlockGrid from_json(const json::Value& value);
    };

    bool operator==(const BlockGrid& left, const BlockGrid& right) noexcept;
    inline bool operator!=(const BlockGrid& left, const BlockGrid& right) noexcept
    {
        return !(left == right);
    }

    LOSSYLAB_REFLECT(BlockGrid, kind, block_width, block_height, phase_x, phase_y, valid);
}
