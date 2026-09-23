#include "lossylab/core/error.hpp"
#include "lossylab/core/geometry.hpp"

#include <array>
#include <cassert>
#include <cmath>

using namespace lossylab;

namespace
{
    constexpr double tol = 1e-9;

    void test_identity_maps_points_unchanged()
    {
        const CoordinateTransform t = CoordinateTransform::identity();
        assert(t.is_identity());
        assert(t.is_integer_translation());

        const Point p = t.map_forward({3.5, 7.25});
        assert(std::abs(p.x - 3.5) < tol);
        assert(std::abs(p.y - 7.25) < tol);
    }

    void test_composition_applies_in_order()
    {
        const CoordinateTransform scale = CoordinateTransform::scaling(2.0, 3.0);
        const CoordinateTransform shift = CoordinateTransform::translation(10.0, 20.0);

        // then() means "scale, and then shift", so the shift is not scaled.
        const Point p = scale.then(shift).map_forward({1.0, 1.0});
        assert(std::abs(p.x - 12.0) < tol);
        assert(std::abs(p.y - 23.0) < tol);

        // The other order scales the shift.
        const Point q = shift.then(scale).map_forward({1.0, 1.0});
        assert(std::abs(q.x - 22.0) < tol);
        assert(std::abs(q.y - 63.0) < tol);
    }

    void test_inverse_undoes_forward()
    {
        const CoordinateTransform t =
            CoordinateTransform::scaling(0.25, 0.5)
                .then(CoordinateTransform::rotation_degrees(30.0))
                .then(CoordinateTransform::translation(-4.0, 9.0));

        const Point original{123.75, -42.5};
        const Point there = t.map_forward(original);
        const Point back = t.map_inverse(there);

        assert(std::abs(back.x - original.x) < 1e-9);
        assert(std::abs(back.y - original.y) < 1e-9);
    }

    void test_singular_transforms_refuse_to_invert()
    {
        const CoordinateTransform degenerate = CoordinateTransform::scaling(1.0, 0.0);
        assert(!degenerate.is_invertible());
        try { (void)degenerate.inverse(); assert(false && "expected throw"); } catch (const ConfigError&) {}
        try { (void)degenerate.map_inverse({1.0, 1.0}); assert(false && "expected throw"); } catch (const ConfigError&) {}
    }

    void test_resize_aligns_image_edges_not_pixel_zero()
    {
        // Downscaling 4x1 to 2x1. Output pixel 0 averages input pixels 0 and 1, so
        // it sits at input coordinate 0.5; output pixel 1 sits at 2.5. A transform
        // with no offset would map both output pixels half a pixel too far left.
        const CoordinateTransform t = CoordinateTransform::resize(4, 1, 2, 1);

        assert(std::abs(t.map_inverse({0.0, 0.0}).x - 0.5) < tol);
        assert(std::abs(t.map_inverse({1.0, 0.0}).x - 2.5) < tol);

        // The image extent maps onto the output extent exactly: the outer edges of
        // the two grids coincide, which is the property that makes it a resize
        // rather than a resize plus a shift.
        assert(std::abs(t.map_forward({-0.5, 0.0}).x - (-0.5)) < tol);
        assert(std::abs(t.map_forward({3.5, 0.0}).x - 1.5) < tol);
    }

    void test_upscaling_is_the_inverse_of_downscaling()
    {
        const CoordinateTransform down = CoordinateTransform::resize(1920, 1080, 480, 270);
        const CoordinateTransform up = CoordinateTransform::resize(480, 270, 1920, 1080);

        // Round-tripping the geometry has to land exactly where it started, or a
        // mask carried through a downscale/upscale pair would drift.
        assert(down.then(up).is_identity());
    }

    void test_resize_by_one_is_the_identity()
    {
        assert(CoordinateTransform::resize(640, 480, 640, 480).is_identity());
    }

    void test_crop_then_resize_traces_back_to_the_source()
    {
        // The case the design cares about: an output crop has to be traceable to
        // the pixels it came from, through every intermediate stage.
        const CoordinateTransform pipeline =
            CoordinateTransform::crop(100.0, 50.0).then(CoordinateTransform::resize(400, 400, 100, 100));

        // Output pixel 0 averages a 4x4 input block: crop pixels 0..3, which are
        // input pixels 100..103, centered at 101.5. Likewise 51.5 vertically.
        const Point source = pipeline.map_inverse({0.0, 0.0});
        assert(std::abs(source.x - 101.5) < tol);
        assert(std::abs(source.y - 51.5) < tol);

        // And the far corner of the output traces back inside the crop, not past it.
        const Point far_corner = pipeline.map_inverse({99.0, 99.0});
        assert(std::abs(far_corner.x - 497.5) < tol);
        assert(std::abs(far_corner.y - 447.5) < tol);
    }

    void test_rect_mapping_bounds_a_rotation()
    {
        const CoordinateTransform t = CoordinateTransform::rotation_degrees(90.0);
        const Rect mapped = t.map_bounds(Rect{0.0, 0.0, 2.0, 4.0});

        assert(std::abs(mapped.width - 4.0) < tol);
        assert(std::abs(mapped.height - 2.0) < tol);
    }

    void test_integer_translation_is_recognized()
    {
        assert(CoordinateTransform::translation(3.0, -7.0).is_integer_translation());
        assert(!CoordinateTransform::translation(3.5, 0.0).is_integer_translation());
        assert(!CoordinateTransform::scaling(2.0, 2.0).is_integer_translation());
        assert(!CoordinateTransform::rotation_degrees(45.0).is_integer_translation());
    }

    void test_transform_json_round_trip()
    {
        const CoordinateTransform t = CoordinateTransform::resize(1920, 1080, 640, 360);
        assert(CoordinateTransform::from_json(t.to_json()) == t);
    }

    // ---------------------------------------------------------------------------
    // Block grids
    // ---------------------------------------------------------------------------

    void test_block_grid_defaults_match_their_kind()
    {
        assert(BlockGrid::for_kind(BlockGridKind::Dct8).block_width == 8);
        assert(BlockGrid::for_kind(BlockGridKind::Macroblock16).block_width == 16);
        assert(BlockGrid::for_kind(BlockGridKind::Ctu32).block_height == 32);
        assert(BlockGrid::for_kind(BlockGridKind::Ctu64).block_height == 64);
    }

    void test_integer_translation_shifts_the_phase_and_keeps_the_grid()
    {
        const BlockGrid grid = BlockGrid::for_kind(BlockGridKind::Dct8);
        const BlockGrid shifted = grid.apply_transform(CoordinateTransform::translation(-3.0, -5.0));

        assert(shifted.valid);
        assert(shifted.phase_x == 5);  // -3 mod 8
        assert(shifted.phase_y == 3);  // -5 mod 8
    }

    void test_whole_block_shifts_leave_the_phase_alone()
    {
        const BlockGrid grid = BlockGrid::for_kind(BlockGridKind::Dct8);
        const BlockGrid shifted = grid.apply_transform(CoordinateTransform::translation(-16.0, 24.0));

        assert(shifted.valid);
        assert(shifted.phase_x == 0);
        assert(shifted.phase_y == 0);
    }

    void test_resampling_destroys_the_grid()
    {
        // The requirement from the design: a later resampling marks the grid as
        // destroyed. It falls out of the transform rather than each stage
        // remembering to say so.
        const BlockGrid grid = BlockGrid::for_kind(BlockGridKind::Dct8);

        assert(!grid.apply_transform(CoordinateTransform::resize(64, 64, 32, 32)).valid);
        assert(!grid.apply_transform(CoordinateTransform::rotation_degrees(5.0)).valid);
        assert(!grid.apply_transform(CoordinateTransform::translation(0.5, 0.0)).valid);
    }

    void test_an_invalid_grid_stays_invalid()
    {
        BlockGrid grid = BlockGrid::for_kind(BlockGridKind::Dct8);
        grid.valid = false;

        // Once destroyed, no later whole-pixel crop brings it back.
        const BlockGrid after = grid.apply_transform(CoordinateTransform::translation(8.0, 8.0));
        assert(!after.valid);
    }

    void test_block_origins_follow_the_phase()
    {
        BlockGrid grid = BlockGrid::for_kind(BlockGridKind::Dct8);
        grid.phase_x = 3;
        grid.phase_y = 0;

        assert(grid.is_block_origin(3, 0));
        assert(grid.is_block_origin(11, 8));
        assert(!grid.is_block_origin(0, 0));

        grid.valid = false;
        assert(!grid.is_block_origin(3, 0));
    }

    void test_orientation_transforms_map_corners_onto_corners()
    {
        // A 4x3 image's top-left pixel, and where each orientation shows it.
        constexpr int width = 4;
        constexpr int height = 3;
        const std::array<Point, 8> expected = {{
            {0, 0}, {3, 0}, {3, 2}, {0, 2}, {0, 0}, {2, 0}, {2, 3}, {0, 3},
        }};
        for (int orientation = 1; orientation <= 8; ++orientation)
        {
            const CoordinateTransform t = CoordinateTransform::orientation(orientation, width, height);
            const Point top_left = t.map_forward({0, 0});
            const Point wanted = expected[static_cast<std::size_t>(orientation - 1)];
            assert(std::abs(top_left.x - wanted.x) < tol && std::abs(top_left.y - wanted.y) < tol);

            // Every corner lands on a corner of the output, never off it.
            const bool transposes = orientation >= 5;
            const Rect bounds = t.map_bounds({0, 0, width - 1, height - 1});
            assert(std::abs(bounds.x) < tol && std::abs(bounds.y) < tol);
            assert(std::abs(bounds.width - (transposes ? height - 1 : width - 1)) < tol);
        }

        // Orientation 6 turns the image clockwise: the top-right pixel ends up
        // at the bottom right.
        const Point top_right = CoordinateTransform::orientation(6, width, height).map_forward({3, 0});
        assert(std::abs(top_right.x - 2) < tol && std::abs(top_right.y - 3) < tol);

        bool threw = false;
        try
        {
            (void)CoordinateTransform::orientation(9, width, height);
        }
        catch (const ConfigError&)
        {
            threw = true;
        }
        assert(threw);
    }

    void test_a_flip_keeps_the_grid_with_the_phase_measured_from_the_far_edge()
    {
        // 20 pixels wide: blocks start at 0, 8 and 16 (a partial block). Mirrored,
        // the partial block's 4 pixels come first, so blocks start at 4.
        const BlockGrid grid = BlockGrid::for_kind(BlockGridKind::Dct8);
        const BlockGrid mirrored = grid.apply_transform(CoordinateTransform::orientation(2, 20, 16));
        assert(mirrored.valid);
        assert(mirrored.phase_x == 4);
        assert(mirrored.phase_y == 0);

        const BlockGrid both = grid.apply_transform(CoordinateTransform::orientation(3, 20, 13));
        assert(both.phase_x == 4);
        assert(both.phase_y == 5);
    }

    void test_a_quarter_turn_swaps_the_grid_axes()
    {
        BlockGrid grid = BlockGrid::for_kind(BlockGridKind::Dct8);
        grid.block_width = 16;
        grid.phase_x = 3;
        grid.phase_y = 1;

        const BlockGrid transposed = grid.apply_transform(CoordinateTransform::orientation(5, 20, 12));
        assert(transposed.valid);
        assert(transposed.block_width == 8 && transposed.block_height == 16);
        assert(transposed.phase_x == 1 && transposed.phase_y == 3);

        // Clockwise: output x runs along the input's rows from the bottom.
        // Blocks along y start at 1 and 9 in 12 rows, so reversed they start
        // at 11 - 8 - 1 + 1 = 3.
        const BlockGrid clockwise = grid.apply_transform(CoordinateTransform::orientation(6, 20, 12));
        assert(clockwise.phase_x == 3 && clockwise.phase_y == 3);

        // A quarter turn built from an angle survives too.
        assert(grid.apply_transform(CoordinateTransform::rotation_degrees(90.0)).valid);
    }

    void test_block_grid_json_round_trip()
    {
        BlockGrid grid = BlockGrid::for_kind(BlockGridKind::Ctu64);
        grid.phase_x = 17;
        grid.valid = false;

        assert(BlockGrid::from_json(grid.to_json()) == grid);
    }
}

int main()
{
    test_identity_maps_points_unchanged();
    test_composition_applies_in_order();
    test_inverse_undoes_forward();
    test_singular_transforms_refuse_to_invert();
    test_resize_aligns_image_edges_not_pixel_zero();
    test_upscaling_is_the_inverse_of_downscaling();
    test_resize_by_one_is_the_identity();
    test_crop_then_resize_traces_back_to_the_source();
    test_rect_mapping_bounds_a_rotation();
    test_integer_translation_is_recognized();
    test_transform_json_round_trip();
    test_block_grid_defaults_match_their_kind();
    test_integer_translation_shifts_the_phase_and_keeps_the_grid();
    test_whole_block_shifts_leave_the_phase_alone();
    test_resampling_destroys_the_grid();
    test_an_invalid_grid_stays_invalid();
    test_block_origins_follow_the_phase();
    test_orientation_transforms_map_corners_onto_corners();
    test_a_flip_keeps_the_grid_with_the_phase_measured_from_the_far_edge();
    test_a_quarter_turn_swaps_the_grid_axes();
    test_block_grid_json_round_trip();
}
