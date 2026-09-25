#include "lossylab/convert/convert.hpp"
#include "lossylab/core/error.hpp"
#include "lossylab/io/decode_image.hpp"

#include <cassert>
#include <cmath>
#include <string>

using namespace lossylab;

namespace
{
    std::string data_path(std::string_view name)
    {
        return std::string(LOSSYLAB_TEST_DATA_DIR) + "/" + std::string(name);
    }

    /// A decoded RGB fixture, fully tagged as sRGB.
    Frame rgb_source()
    {
        return decode_image(Source::from_path(data_path("testsrc_64x48.png"))).frame;
    }

    /// A YUV preset's matrix, range and siting with the sRGB source's primaries
    /// and transfer, which convert() cannot change.
    ColorSpec keeping_source_gamut(ColorSpec target)
    {
        target.primaries = ColorPrimaries::Bt709;
        target.transfer = TransferCharacteristic::Srgb;
        return target;
    }

    ColorSpec bt709_yuv() { return keeping_source_gamut(ColorSpec::bt709_limited()); }
    ColorSpec bt709_full_yuv() { return keeping_source_gamut(ColorSpec::bt709_full()); }
    ColorSpec bt601_yuv() { return keeping_source_gamut(ColorSpec::bt601_limited()); }

    bool has_conversion(const ConversionList& conversions, const std::string& property)
    {
        for (const ConversionEvent& conversion : conversions)
        {
            if (conversion.property == property)
            {
                return true;
            }
        }
        return false;
    }

    /// Mean absolute difference between two frames' samples on one plane.
    double plane_difference(const Frame& left, const Frame& right, const int plane = 0)
    {
        const ConstPlaneView left_plane = left.plane(plane);
        const ConstPlaneView right_plane = right.plane(plane);
        double total = 0.0;
        long count = 0;
        for (int row = 0; row < left_plane.height; ++row)
        {
            for (int byte = 0; byte < left_plane.row_bytes(); ++byte)
            {
                total += std::abs(static_cast<int>(left_plane.row(row)[byte]) -
                                  static_cast<int>(right_plane.row(row)[byte]));
                ++count;
            }
        }
        return count == 0 ? 0.0 : total / static_cast<double>(count);
    }

    void test_converting_rgb_to_yuv_produces_the_requested_format()
    {
        const Frame source = rgb_source();
        const FrameResult result =
            convert(source, PixelFormat::from_name("yuv420p"), bt709_yuv());

        assert(result.frame.pixel_format().name() == std::string("yuv420p"));
        assert(result.frame.color() == bt709_yuv());

        // Geometry is untouched; that is resize's job, not convert's.
        assert(result.frame.width() == source.width());
        assert(result.frame.height() == source.height());
        assert(result.record.transform.is_identity());
    }

    void test_the_record_describes_both_ends_accurately()
    {
        const Frame source = rgb_source();
        const FrameResult result =
            convert(source, PixelFormat::from_name("yuv422p"), bt709_yuv());

        assert(result.record.input == source.describe());
        assert(result.record.output == result.frame.describe());
        assert(result.record.kind == StageKind::Convert);
        assert(result.record.implementation == std::string("swscale"));
    }

    void test_format_subsampling_and_depth_changes_are_all_recorded()
    {
        const Frame source = rgb_source();
        const FrameResult result =
            convert(source, PixelFormat::from_name("yuv420p10le"), bt709_yuv());

        // Each axis is reported separately, so a record can be grouped on any of
        // them across a dataset.
        assert(has_conversion(result.record.conversions, "pix_fmt"));
        assert(has_conversion(result.record.conversions, "subsampling"));
        assert(has_conversion(result.record.conversions, "bit_depth"));
    }

    void test_strict_mode_allows_the_matrix_change_an_rgb_to_yuv_switch_entails()
    {
        // Going from RGB to YUV necessarily leaves the RGB identity matrix behind
        // and gives the chroma planes a siting, so even the strictest setting has
        // to permit those two. The target keeps every other field of the source, so
        // the entailed changes are the only ones on the table.
        const Frame source = rgb_source();

        ColorSpec target = source.color();
        target.matrix = ColorMatrix::Bt709;
        target.chroma_location = ChromaLocation::Left;

        (void)convert(source, PixelFormat::from_name("yuv420p"), target, Strict::Refuse);
    }

    void test_converting_is_permissive_by_default_because_nothing_is_hidden()
    {
        // The caller named the target format and the target color, so there is
        // nothing to refuse. Strict::Refuse is an opt-in assertion, not the default.
        const Frame source = rgb_source();
        (void)convert(source, PixelFormat::from_name("yuv420p"), bt709_yuv());

        try
        {
            (void)convert(source, PixelFormat::from_name("yuv420p"), bt709_yuv(),
                          Strict::Refuse);
            assert(false && "expected throw");
        }
        catch (const ConversionRefused&)
        {
        }
    }

    void test_strict_mode_refuses_a_color_change_that_was_not_entailed()
    {
        // Within YUV, changing the matrix is a real color decision rather than a
        // consequence of the layout, so the default refuses it.
        const Frame yuv =
            convert(rgb_source(), PixelFormat::from_name("yuv420p"), bt709_yuv())
                .frame;

        ConvertOptions options;
        options.pixel_format = PixelFormat::from_name("yuv420p");
        options.color = bt601_yuv();
        options.strict = Strict::Refuse;

        try
        {
            (void)convert(yuv, options);
            assert(false && "expected throw");
        }
        catch (const ConversionRefused&)
        {
        }
    }

    void test_strict_mode_refuses_a_range_change()
    {
        const Frame yuv =
            convert(rgb_source(), PixelFormat::from_name("yuv420p"), bt709_yuv())
                .frame;

        ConvertOptions options;
        options.pixel_format = PixelFormat::from_name("yuv420p");
        options.color = bt709_full_yuv();
        options.strict = Strict::Refuse;

        try
        {
            (void)convert(yuv, options);
            assert(false && "expected throw");
        }
        catch (const ConversionRefused&)
        {
        }
    }

    void test_allow_recorded_performs_the_same_conversion_and_logs_it()
    {
        const Frame yuv =
            convert(rgb_source(), PixelFormat::from_name("yuv420p"), bt709_yuv())
                .frame;

        ConvertOptions options;
        options.pixel_format = PixelFormat::from_name("yuv420p");
        options.color = bt601_yuv();
        options.strict = Strict::AllowRecorded;

        const FrameResult result = convert(yuv, options);

        assert(result.frame.color() == bt601_yuv());
        assert(has_conversion(result.record.conversions, "color_matrix"));
    }

    void test_a_gamut_or_tone_curve_change_is_refused_under_any_policy()
    {
        // swscale would apply the matrix and range and leave the samples in
        // their old gamut, while the output claimed the new one.
        const Frame source = rgb_source();
        for (const Strict strict : {Strict::AllowRecorded, Strict::Refuse})
        {
            for (const ColorSpec& target : {ColorSpec::bt709_limited(), ColorSpec::bt601_limited()})
            {
                try
                {
                    (void)convert(source, PixelFormat::from_name("yuv420p"), target, strict);
                    assert(false && "expected throw");
                }
                catch (const NotImplemented&)
                {
                }
            }
        }
    }

    void test_a_chroma_roundtrip_keeps_the_source_gamut_whatever_color_it_names()
    {
        // The default names BT.709 limited, whose transfer differs from the
        // sRGB source's; only its matrix, range and siting are used.
        ChromaRoundtripOptions options;
        options.color = ColorSpec::bt601_limited();
        const FrameResult result = chroma_roundtrip(rgb_source(), options);
        assert(!has_conversion(result.record.conversions, "primaries"));
        assert(!has_conversion(result.record.conversions, "transfer"));
        assert(has_conversion(result.record.conversions, "color_matrix"));
    }

    void test_an_unspecified_color_is_refused_rather_than_guessed()
    {
        const Frame source = rgb_source();

        ConvertOptions options;
        options.pixel_format = PixelFormat::from_name("yuv420p");
        options.color = ColorSpec();  // nothing specified

        try
        {
            (void)convert(source, options);
            assert(false && "expected throw");
        }
        catch (const ConfigError&)
        {
        }
    }

    void test_convert_rejects_empty_frames_and_invalid_targets()
    {
        try
        {
            (void)convert(Frame(), PixelFormat::from_name("yuv420p"),
                          bt709_yuv());
            assert(false && "expected throw");
        }
        catch (const ConfigError&)
        {
        }

        ConvertOptions options;
        options.pixel_format = PixelFormat();
        options.color = bt709_yuv();
        try
        {
            (void)convert(rgb_source(), options);
            assert(false && "expected throw");
        }
        catch (const ConfigError&)
        {
        }
    }

    void test_the_kernel_used_is_named_in_the_record()
    {
        const Frame source = rgb_source();

        ConvertOptions options;
        options.pixel_format = PixelFormat::from_name("yuv420p");
        options.color = bt709_yuv();
        options.chroma_down = KernelSpec{Kernel::Lanczos, {}};

        const FrameResult result = convert(source, options);

        // 4:4:4 RGB to 4:2:0 shrinks chroma, so the downsampling kernel applies and
        // the record has to say which one actually ran.
        assert(result.configuration.at("kernel_role").get<std::string>() ==
              std::string("chroma_down"));
        assert(result.configuration.at("kernel").at("kernel").get<std::string>() ==
              std::string("lanczos"));
    }

    void test_different_chroma_kernels_give_different_results()
    {
        // Kernel choice is one of the strongest traces a conversion leaves, so two
        // kernels must not quietly produce the same output.
        const Frame source = rgb_source();

        ConvertOptions area;
        area.pixel_format = PixelFormat::from_name("yuv420p");
        area.color = bt709_yuv();
        area.chroma_down = KernelSpec{Kernel::Area, {}};

        ConvertOptions point = area;
        point.chroma_down = KernelSpec{Kernel::Nearest, {}};

        const Frame with_area = convert(source, area).frame;
        const Frame with_point = convert(source, point).frame;

        // The chroma planes are where the kernel acts: luma is computed per pixel
        // and is identical either way, so comparing plane 0 would prove nothing.
        assert(std::abs(plane_difference(with_area, with_point, 0) - 0.0) < 1e-12);
        assert(plane_difference(with_area, with_point, 1) > 0.0);
    }

    // -----------------------------------------------------------------------
    // chroma_roundtrip
    // -----------------------------------------------------------------------

    void test_a_chroma_roundtrip_returns_to_the_source_format()
    {
        const Frame source = rgb_source();

        ChromaRoundtripOptions options;
        options.subsampling = Subsampling::Yuv420;

        const FrameResult result = chroma_roundtrip(source, options);

        // Same format and color it started in: the only thing left behind is the
        // chroma information the subsampling discarded.
        assert(result.frame.pixel_format() == source.pixel_format());
        assert(result.frame.color() == source.color());
        assert(result.frame.width() == source.width());
        assert(result.record.kind == StageKind::ChromaRoundtrip);
    }

    void test_a_chroma_roundtrip_actually_loses_chroma()
    {
        // If the round trip were lossless it would be applying no history at all,
        // which would make it useless for both augmentation and equalization.
        const Frame source = rgb_source();

        ChromaRoundtripOptions options;
        options.subsampling = Subsampling::Yuv420;

        const Frame result = chroma_roundtrip(source, options).frame;
        assert(plane_difference(source, result) > 0.0);
    }

    void test_coarser_subsampling_loses_more()
    {
        const Frame source = rgb_source();

        ChromaRoundtripOptions through_444;
        through_444.subsampling = Subsampling::Yuv444;

        ChromaRoundtripOptions through_420;
        through_420.subsampling = Subsampling::Yuv420;

        const double loss_444 =
            plane_difference(source, chroma_roundtrip(source, through_444).frame);
        const double loss_420 =
            plane_difference(source, chroma_roundtrip(source, through_420).frame);

        assert(loss_420 > loss_444);
    }

    void test_both_legs_of_the_roundtrip_are_recorded()
    {
        const Frame source = rgb_source();

        ChromaRoundtripOptions options;
        options.subsampling = Subsampling::Yuv420;

        const FrameResult result = chroma_roundtrip(source, options);

        // Reporting only the endpoints would hide that anything happened, since
        // the frame comes back in the format it started in.
        assert(result.record.conversions.size() >= 2);
        assert(result.configuration.at("intermediate_pix_fmt").get<std::string>() ==
              std::string("yuv420p"));
    }

    void test_a_roundtrip_needs_a_chroma_bearing_subsampling()
    {
        const Frame source = rgb_source();

        ChromaRoundtripOptions options;
        options.subsampling = Subsampling::Rgb;
        try
        {
            (void)chroma_roundtrip(source, options);
            assert(false && "expected throw");
        }
        catch (const ConfigError&)
        {
        }

        options.subsampling = Subsampling::Gray;
        try
        {
            (void)chroma_roundtrip(source, options);
            assert(false && "expected throw");
        }
        catch (const ConfigError&)
        {
        }
    }

    // -----------------------------------------------------------------------
    // reinterpret
    // -----------------------------------------------------------------------

    void test_reinterpret_changes_the_label_and_nothing_else()
    {
        const Frame yuv =
            convert(rgb_source(), PixelFormat::from_name("yuv420p"), bt709_yuv())
                .frame;

        const FrameResult result = reinterpret(yuv, bt601_yuv());

        assert(result.frame.color() == bt601_yuv());
        assert(result.frame.pixel_format() == yuv.pixel_format());

        // Not one sample may differ: this models a lost or misread tag, not a
        // conversion.
        assert(std::abs(plane_difference(yuv, result.frame) - 0.0) < 1e-12);
        assert(!result.configuration.at("samples_modified").get<bool>());
    }

    void test_reinterpret_records_every_field_it_relabeled()
    {
        const Frame yuv =
            convert(rgb_source(), PixelFormat::from_name("yuv420p"), bt709_yuv())
                .frame;

        const FrameResult result = reinterpret(yuv, bt601_yuv());

        // Lossless is not the same as invisible: every downstream stage reads these
        // samples differently now, so the relabeling has to be in the record.
        assert(has_conversion(result.record.conversions, "color_matrix"));
        assert(result.record.kind == StageKind::Reinterpret);
    }

    void test_reinterpret_is_reversible()
    {
        const Frame yuv =
            convert(rgb_source(), PixelFormat::from_name("yuv420p"), bt709_yuv())
                .frame;

        const Frame there = reinterpret(yuv, bt601_yuv()).frame;
        const Frame back = reinterpret(there, bt709_yuv()).frame;

        assert(back.color() == yuv.color());
        assert(std::abs(plane_difference(yuv, back) - 0.0) < 1e-12);
    }

    void test_relabeling_to_the_same_color_records_nothing()
    {
        const Frame yuv =
            convert(rgb_source(), PixelFormat::from_name("yuv420p"), bt709_yuv())
                .frame;

        const FrameResult result = reinterpret(yuv, bt709_yuv());
        assert(result.record.conversions.empty());
    }

    void test_reinterpret_and_convert_differ_where_it_matters()
    {
        // The distinction the API is built around: convert changes the numbers so
        // the color stays the same; reinterpret changes the color by leaving the
        // numbers alone. Confusing them is a real pipeline bug, and the two must
        // not produce the same frame.
        const Frame yuv =
            convert(rgb_source(), PixelFormat::from_name("yuv420p"), bt709_yuv())
                .frame;

        ConvertOptions options;
        options.pixel_format = PixelFormat::from_name("yuv420p");
        options.color = bt601_yuv();
        options.strict = Strict::AllowRecorded;

        const Frame converted = convert(yuv, options).frame;
        const Frame relabeled = reinterpret(yuv, bt601_yuv()).frame;

        assert(converted.color() == relabeled.color());
        assert(plane_difference(converted, relabeled) > 0.0);
    }
}

int main()
{
    test_converting_rgb_to_yuv_produces_the_requested_format();
    test_the_record_describes_both_ends_accurately();
    test_format_subsampling_and_depth_changes_are_all_recorded();
    test_strict_mode_allows_the_matrix_change_an_rgb_to_yuv_switch_entails();
    test_converting_is_permissive_by_default_because_nothing_is_hidden();
    test_strict_mode_refuses_a_color_change_that_was_not_entailed();
    test_strict_mode_refuses_a_range_change();
    test_allow_recorded_performs_the_same_conversion_and_logs_it();
    test_a_gamut_or_tone_curve_change_is_refused_under_any_policy();
    test_a_chroma_roundtrip_keeps_the_source_gamut_whatever_color_it_names();
    test_an_unspecified_color_is_refused_rather_than_guessed();
    test_convert_rejects_empty_frames_and_invalid_targets();
    test_the_kernel_used_is_named_in_the_record();
    test_different_chroma_kernels_give_different_results();
    test_a_chroma_roundtrip_returns_to_the_source_format();
    test_a_chroma_roundtrip_actually_loses_chroma();
    test_coarser_subsampling_loses_more();
    test_both_legs_of_the_roundtrip_are_recorded();
    test_a_roundtrip_needs_a_chroma_bearing_subsampling();
    test_reinterpret_changes_the_label_and_nothing_else();
    test_reinterpret_records_every_field_it_relabeled();
    test_reinterpret_is_reversible();
    test_relabeling_to_the_same_color_records_nothing();
    test_reinterpret_and_convert_differ_where_it_matters();
}
