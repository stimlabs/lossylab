#include "lossylab/core/error.hpp"
#include "lossylab/core/frame.hpp"

#include <cassert>
#include <cmath>

using namespace lossylab;

namespace
{
    Frame make_frame(const char* pix_fmt = "yuv420p", const int width = 64,
                     const int height = 48)
    {
        return Frame::allocate(width, height, PixelFormat::from_name(pix_fmt),
                               ColorSpec::bt709_limited());
    }

    void test_a_default_frame_is_empty()
    {
        const Frame frame;
        assert(frame.empty());
        assert(!frame);
        assert(frame.width() == 0);
        assert(frame.plane_count() == 0);
    }

    void test_allocation_sets_geometry_and_color()
    {
        const Frame frame = make_frame();

        assert(!frame.empty());
        assert(frame.width() == 64);
        assert(frame.height() == 48);
        assert(frame.pixel_format().name() == std::string("yuv420p"));
        assert(frame.color() == ColorSpec::bt709_limited());
        assert(frame.plane_count() == 3);
    }

    void test_allocation_rejects_nonsense()
    {
        try
        {
            (void)Frame::allocate(0, 48, PixelFormat::from_name("yuv420p"),
                                  ColorSpec::bt709_limited());
            assert(false && "expected throw");
        }
        catch (const ConfigError&)
        {
        }

        try
        {
            (void)Frame::allocate(64, -1, PixelFormat::from_name("yuv420p"),
                                  ColorSpec::bt709_limited());
            assert(false && "expected throw");
        }
        catch (const ConfigError&)
        {
        }

        try
        {
            (void)Frame::allocate(64, 48, PixelFormat(), ColorSpec::bt709_limited());
            assert(false && "expected throw");
        }
        catch (const ConfigError&)
        {
        }
    }

    void test_plane_geometry_follows_the_subsampling()
    {
        const Frame frame = make_frame("yuv420p", 64, 48);

        const ConstPlaneView luma = frame.plane(0);
        assert(luma.width == 64);
        assert(luma.height == 48);
        assert(luma.bytes_per_sample == 1);

        // Chroma is half in each direction for 4:2:0.
        const ConstPlaneView chroma = frame.plane(1);
        assert(chroma.width == 32);
        assert(chroma.height == 24);

        // Stride is a byte count and may exceed the row length, which is exactly
        // why the views carry it instead of assuming packed rows.
        assert(luma.stride >= luma.row_bytes());
    }

    void test_odd_dimensions_round_chroma_up()
    {
        // A 65x49 4:2:0 frame has 33x25 chroma planes, not 32x24: rounding down
        // would drop the last column and row.
        const Frame frame = make_frame("yuv420p", 65, 49);
        const ConstPlaneView chroma = frame.plane(1);
        assert(chroma.width == 33);
        assert(chroma.height == 25);
    }

    void test_high_bit_depth_planes_report_two_bytes_per_sample()
    {
        const Frame frame = make_frame("yuv420p10le");
        assert(frame.plane(0).bytes_per_sample == 2);
        assert(frame.plane(0).row_bytes() == 128);
    }

    void test_packed_formats_have_one_plane_with_interleaved_components()
    {
        const Frame frame = make_frame("rgb24", 64, 48);

        assert(frame.plane_count() == 1);
        const ConstPlaneView plane = frame.plane(0);
        assert(plane.components_per_pixel == 3);
        assert(plane.row_bytes() == 64 * 3);
    }

    void test_plane_index_is_bounds_checked()
    {
        const Frame frame = make_frame();
        try
        {
            (void)frame.plane(-1);
            assert(false && "expected throw");
        }
        catch (const ConfigError&)
        {
        }
        try
        {
            (void)frame.plane(3);
            assert(false && "expected throw");
        }
        catch (const ConfigError&)
        {
        }

        const Frame empty;
        try
        {
            (void)empty.plane(0);
            assert(false && "expected throw");
        }
        catch (const ConfigError&)
        {
        }
    }

    void test_copies_share_buffers_and_are_not_writable()
    {
        Frame original = make_frame();
        assert(original.is_writable());

        const Frame copy = original;

        // Both now reference the same buffers, so neither may be written through
        // without saying so first.
        assert(!original.is_writable());
        assert(!copy.is_writable());
        try
        {
            (void)original.plane(0);
            assert(false && "expected throw");
        }
        catch (const ConfigError&)
        {
        }
    }

    void test_make_writable_detaches_from_a_shared_buffer()
    {
        Frame original = make_frame();
        const ConstPlaneView before = original.plane(0);

        const Frame copy = original;
        original.make_writable();

        assert(original.is_writable());
        // Detaching means new storage, so writing through it cannot reach the copy.
        assert(original.plane(0).data != before.data);
        assert(copy.plane(0).data == before.data);
    }

    void test_writes_through_a_detached_frame_do_not_reach_the_copy()
    {
        Frame original = make_frame();
        original.plane(0).row(0)[0] = 42;

        Frame copy = original;
        copy.make_writable();
        copy.plane(0).row(0)[0] = 99;

        assert(original.plane(0).row(0)[0] == std::uint8_t{42});
        assert(copy.plane(0).row(0)[0] == std::uint8_t{99});
    }

    void test_clone_is_an_independent_deep_copy()
    {
        Frame original = make_frame();
        original.plane(0).row(0)[0] = 7;

        Frame copy = original.clone();
        assert(copy.is_writable());
        assert(original.is_writable());
        assert(copy.plane(0).row(0)[0] == std::uint8_t{7});

        copy.plane(0).row(0)[0] = 8;
        assert(original.plane(0).row(0)[0] == std::uint8_t{7});
    }

    void test_cloning_an_empty_frame_gives_an_empty_frame()
    {
        const Frame empty;
        assert(empty.clone().empty());
    }

    void test_moves_leave_the_source_empty()
    {
        Frame original = make_frame();
        const Frame moved = std::move(original);

        assert(!moved.empty());
        assert(original.empty());
        // A moved-from frame is still writable-checkable rather than a trap.
        assert(!original.is_writable());
    }

    void test_set_color_relabels_without_touching_samples()
    {
        Frame frame = make_frame();
        frame.plane(0).row(0)[0] = 128;

        frame.set_color(ColorSpec::bt601_limited());
        frame.sync_color_to_av_frame();

        assert(frame.color() == ColorSpec::bt601_limited());
        assert(frame.plane(0).row(0)[0] == std::uint8_t{128});
    }

    void test_describe_matches_the_frame()
    {
        const Frame frame = make_frame("yuv422p", 32, 16);
        const FormatDescription description = frame.describe();

        assert(description.width == 32);
        assert(description.height == 16);
        assert(description.pixel_format == frame.pixel_format());
        assert(description.color == frame.color());
    }

    void test_timestamps_need_a_time_base_to_be_meaningful()
    {
        Frame frame = make_frame();
        frame.set_pts(90000);

        // A timestamp without a time base is uninterpretable, and is reported as
        // absent rather than guessed at.
        assert(!frame.timestamp_seconds().has_value());

        frame.set_time_base(Rational{1, 90000});
        assert(frame.timestamp_seconds().has_value());
        assert(std::abs(*frame.timestamp_seconds() - 1.0) < 1e-12);
    }

    void test_a_frame_without_side_data_has_no_qp_map()
    {
        assert(!make_frame().qp_map().has_value());
        assert(!Frame().qp_map().has_value());
    }

    // -----------------------------------------------------------------------
    // QpMap
    // -----------------------------------------------------------------------

    QpMap make_qp_map()
    {
        // 4x2 blocks of 16x16 pixels, quantizer rising left to right.
        QpMap map;
        map.width = 4;
        map.height = 2;
        map.block_width = 16;
        map.block_height = 16;
        map.values = {10, 20, 30, 40, 10, 20, 30, 40};
        return map;
    }

    void test_qp_map_indexing_is_bounds_checked()
    {
        const QpMap map = make_qp_map();
        assert(map.at(0, 0) == 10);
        assert(map.at(3, 1) == 40);
        try
        {
            (void)map.at(4, 0);
            assert(false && "expected throw");
        }
        catch (const ConfigError&)
        {
        }
        try
        {
            (void)map.at(0, 2);
            assert(false && "expected throw");
        }
        catch (const ConfigError&)
        {
        }
        try
        {
            (void)map.at(-1, 0);
            assert(false && "expected throw");
        }
        catch (const ConfigError&)
        {
        }
    }

    void test_qp_mean_over_a_crop_covers_every_block_it_touches()
    {
        const QpMap map = make_qp_map();

        // Exactly one block.
        assert(std::abs(map.mean_over(Rect{0, 0, 16, 16}) - 10.0) < 1e-12);

        // A crop straddling two blocks carries both, however little of the second
        // it covers: that block's quantization is in those pixels either way.
        assert(std::abs(map.mean_over(Rect{0, 0, 17, 16}) - 15.0) < 1e-12);

        // The whole map.
        assert(std::abs(map.mean_over(Rect{0, 0, 64, 32}) - 25.0) < 1e-12);
    }

    void test_qp_mean_rejects_an_empty_map_or_an_outside_rect()
    {
        try
        {
            (void)QpMap{}.mean_over(Rect{0, 0, 16, 16});
            assert(false && "expected throw");
        }
        catch (const ConfigError&)
        {
        }
        try
        {
            (void)make_qp_map().mean_over(Rect{1000, 1000, 16, 16});
            assert(false && "expected throw");
        }
        catch (const ConfigError&)
        {
        }
    }
}

int main()
{
    test_a_default_frame_is_empty();
    test_allocation_sets_geometry_and_color();
    test_allocation_rejects_nonsense();
    test_plane_geometry_follows_the_subsampling();
    test_odd_dimensions_round_chroma_up();
    test_high_bit_depth_planes_report_two_bytes_per_sample();
    test_packed_formats_have_one_plane_with_interleaved_components();
    test_plane_index_is_bounds_checked();
    test_copies_share_buffers_and_are_not_writable();
    test_make_writable_detaches_from_a_shared_buffer();
    test_writes_through_a_detached_frame_do_not_reach_the_copy();
    test_clone_is_an_independent_deep_copy();
    test_cloning_an_empty_frame_gives_an_empty_frame();
    test_moves_leave_the_source_empty();
    test_set_color_relabels_without_touching_samples();
    test_describe_matches_the_frame();
    test_timestamps_need_a_time_base_to_be_meaningful();
    test_a_frame_without_side_data_has_no_qp_map();
    test_qp_map_indexing_is_bounds_checked();
    test_qp_mean_over_a_crop_covers_every_block_it_touches();
    test_qp_mean_rejects_an_empty_map_or_an_outside_rect();
}
