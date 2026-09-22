#include "lossylab/core/error.hpp"
#include "lossylab/core/pixel_format.hpp"

#include <cassert>

using namespace lossylab;

namespace
{
    void test_a_default_pixel_format_is_invalid()
    {
        const PixelFormat none;
        assert(!none.is_valid());
        assert(none.name() == std::string("none"));
        assert(none.bit_depth() == 0);
        assert(none.plane_count() == 0);
    }

    void test_formats_resolve_by_name()
    {
        const PixelFormat yuv420p = PixelFormat::from_name("yuv420p");
        assert(yuv420p.is_valid());
        assert(yuv420p.name() == std::string("yuv420p"));
        assert(yuv420p.bit_depth() == 8);
        assert(yuv420p.plane_count() == 3);
        assert(yuv420p.is_planar());
        assert(!yuv420p.is_rgb());
        assert(!yuv420p.has_alpha());
    }

    void test_unknown_names_are_rejected()
    {
        try { (void)PixelFormat::from_name("yuv420q"); assert(false && "expected throw"); }
        catch (const ConfigError&) {}

        assert(!PixelFormat::find("yuv420q").has_value());
        assert(PixelFormat::find("rgb24").has_value());
    }

    void test_subsampling_is_derived_from_the_descriptor()
    {
        assert(PixelFormat::from_name("yuv444p").subsampling() == Subsampling::Yuv444);
        assert(PixelFormat::from_name("yuv422p").subsampling() == Subsampling::Yuv422);
        assert(PixelFormat::from_name("yuv420p").subsampling() == Subsampling::Yuv420);
        assert(PixelFormat::from_name("yuv411p").subsampling() == Subsampling::Yuv411);
        assert(PixelFormat::from_name("yuv410p").subsampling() == Subsampling::Yuv410);
        assert(PixelFormat::from_name("rgb24").subsampling() == Subsampling::Rgb);
        assert(PixelFormat::from_name("gray").subsampling() == Subsampling::Gray);
    }

    void test_chroma_plane_sizes_match_the_subsampling()
    {
        const PixelFormat yuv420p = PixelFormat::from_name("yuv420p");
        assert(yuv420p.log2_chroma_width() == 1);
        assert(yuv420p.log2_chroma_height() == 1);

        const PixelFormat yuv422p = PixelFormat::from_name("yuv422p");
        assert(yuv422p.log2_chroma_width() == 1);
        assert(yuv422p.log2_chroma_height() == 0);

        const PixelFormat yuv444p = PixelFormat::from_name("yuv444p");
        assert(yuv444p.log2_chroma_width() == 0);
        assert(yuv444p.log2_chroma_height() == 0);
    }

    void test_high_bit_depth_formats_report_their_depth()
    {
        assert(PixelFormat::from_name("yuv420p10le").bit_depth() == 10);
        assert(PixelFormat::from_name("yuv444p12le").bit_depth() == 12);
        assert(PixelFormat::from_name("rgb48le").bit_depth() == 16);

        // Depth and subsampling are independent axes.
        assert(PixelFormat::from_name("yuv420p10le").subsampling() == Subsampling::Yuv420);
    }

    void test_alpha_does_not_make_a_format_gray()
    {
        const PixelFormat ya8 = PixelFormat::from_name("ya8");
        assert(ya8.has_alpha());
        assert(ya8.is_gray());

        const PixelFormat rgba = PixelFormat::from_name("rgba");
        assert(rgba.has_alpha());
        assert(!rgba.is_gray());
        assert(rgba.is_rgb());
    }

    void test_planar_yuv_builds_the_canonical_format()
    {
        assert(PixelFormat::planar_yuv(Subsampling::Yuv420, 8).name() == std::string("yuv420p"));
        assert(PixelFormat::planar_yuv(Subsampling::Yuv420, 10).name() ==
               std::string("yuv420p10le"));
        assert(PixelFormat::planar_yuv(Subsampling::Yuv444, 12).name() ==
               std::string("yuv444p12le"));
        assert(PixelFormat::planar_yuv(Subsampling::Gray, 8).name() == std::string("gray"));
        assert(PixelFormat::planar_yuv(Subsampling::Gray, 10).name() == std::string("gray10le"));
        assert(PixelFormat::planar_yuv(Subsampling::Yuv420, 8, true).name() ==
               std::string("yuva420p"));
    }

    void test_planar_yuv_rejects_combinations_that_do_not_exist()
    {
        try { (void)PixelFormat::planar_yuv(Subsampling::Rgb, 8); assert(false && "expected throw"); }
        catch (const ConfigError&) {}

        try { (void)PixelFormat::planar_yuv(Subsampling::Yuv420, 7); assert(false && "expected throw"); }
        catch (const ConfigError&) {}

        try { (void)PixelFormat::planar_yuv(Subsampling::Gray, 8, true); assert(false && "expected throw"); }
        catch (const ConfigError&) {}
    }

    void test_planar_yuv_round_trips_through_subsampling()
    {
        // The property chroma_roundtrip depends on: asking for a subsampling and
        // reading it back must agree, or an augmentation would be mislabeled.
        for (const Subsampling s : {Subsampling::Yuv444, Subsampling::Yuv422, Subsampling::Yuv420,
                                    Subsampling::Yuv411, Subsampling::Yuv410})
        {
            for (const int depth : {8, 10, 12})
            {
                const std::optional<PixelFormat> fmt = [&]() -> std::optional<PixelFormat> {
                    try { return PixelFormat::planar_yuv(s, depth); }
                    catch (const ConfigError&) { return std::nullopt; }
                }();
                if (!fmt) { continue; }  // Not every depth exists for every layout.

                assert(fmt->subsampling() == s);
                assert(fmt->bit_depth() == depth);
            }
        }
    }

    void test_subsampling_names_round_trip()
    {
        for (const Subsampling s : {Subsampling::Rgb, Subsampling::Gray, Subsampling::Yuv444,
                                    Subsampling::Yuv440, Subsampling::Yuv422, Subsampling::Yuv420,
                                    Subsampling::Yuv411, Subsampling::Yuv410})
        {
            assert(subsampling_from_string(to_string(s)) == s);
        }

        // Colon-separated spellings are accepted because specs get written by hand.
        assert(subsampling_from_string("4:2:0") == Subsampling::Yuv420);

        try { (void)subsampling_from_string("4:2:5"); assert(false && "expected throw"); }
        catch (const ConfigError&) {}
    }

    void test_json_round_trip()
    {
        const PixelFormat fmt = PixelFormat::from_name("yuv422p10le");
        assert(PixelFormat::from_json(fmt.to_json()) == fmt);

        const PixelFormat none;
        assert(PixelFormat::from_json(none.to_json()) == none);
    }

    void test_all_formats_are_enumerable()
    {
        const std::vector<PixelFormat> formats = PixelFormat::all();
        assert(formats.size() > 50);
        for (const PixelFormat& fmt : formats)
        {
            assert(fmt.is_valid());
        }
    }
}

int main()
{
    test_a_default_pixel_format_is_invalid();
    test_formats_resolve_by_name();
    test_unknown_names_are_rejected();
    test_subsampling_is_derived_from_the_descriptor();
    test_chroma_plane_sizes_match_the_subsampling();
    test_high_bit_depth_formats_report_their_depth();
    test_alpha_does_not_make_a_format_gray();
    test_planar_yuv_builds_the_canonical_format();
    test_planar_yuv_rejects_combinations_that_do_not_exist();
    test_planar_yuv_round_trips_through_subsampling();
    test_subsampling_names_round_trip();
    test_json_round_trip();
    test_all_formats_are_enumerable();
}
