#include "lossylab/convert/convert.hpp"
#include "lossylab/core/error.hpp"
#include "lossylab/io/decode_image.hpp"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>

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
        assert(result.record.kind() == StageKind::Convert);
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
        assert(std::get<ConvertEvidence>(result.record.evidence).kernel_role == "chroma_down");
        assert(std::get<ConvertOptions>(result.configuration).chroma_down.kernel == Kernel::Lanczos);
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
    // The rgb24 path
    // -----------------------------------------------------------------------

    /// An rgb24 frame whose channels vary independently, tagged sRGB.
    Frame textured_rgb(const int width, const int height)
    {
        Frame frame = Frame::allocate(width, height, PixelFormat::from_name("rgb24"), ColorSpec::srgb());
        for (int y = 0; y < height; ++y)
        {
            std::uint8_t* row = frame.plane(0).row(y);
            for (int x = 0; x < width; ++x)
            {
                row[x * 3] = static_cast<std::uint8_t>((x * 37 + y * 11) % 256);
                row[x * 3 + 1] = static_cast<std::uint8_t>((x * 5 + y * 53) % 256);
                row[x * 3 + 2] = static_cast<std::uint8_t>((x * y + 91) % 256);
            }
        }
        return frame;
    }

    ConvertOptions to_srgb24()
    {
        ConvertOptions options;
        options.pixel_format = PixelFormat::from_name("rgb24");
        options.color = ColorSpec::srgb();
        return options;
    }

    bool samples_equal(const Frame& left, const Frame& right)
    {
        return left.width() == right.width() && left.height() == right.height() &&
               plane_difference(left, right) == 0.0;
    }

    void test_the_chroma_upsampler_asked_for_is_used_at_every_size()
    {
        // swscale's own shortcut repeats 4:2:0 chroma for even sizes whatever
        // the kernel; with it bypassed, two kernels differ at every size.
        for (const auto& [width, height] : {std::pair{64, 48}, std::pair{63, 48}, std::pair{64, 47}})
        {
            const Frame yuv =
                convert(textured_rgb(width, height), PixelFormat::from_name("yuv420p"), bt709_yuv()).frame;
            ConvertOptions nearest = to_srgb24();
            nearest.chroma_up = KernelSpec{Kernel::Nearest, {}};
            ConvertOptions bicubic = to_srgb24();
            bicubic.chroma_up = KernelSpec{Kernel::Bicubic, {}};
            assert(plane_difference(convert(yuv, nearest).frame, convert(yuv, bicubic).frame) > 0.0);
        }
    }

    void test_eight_bit_rgb_gray_and_palette_sources_keep_their_values()
    {
        const Frame rgb = textured_rgb(33, 17);
        const FrameResult same = convert(rgb, to_srgb24());
        assert(samples_equal(same.frame, rgb));
        assert(same.frame.plane(0).stride == 33 * 3);

        Frame gray = Frame::allocate(256, 2, PixelFormat::from_name("gray"), ColorSpec::srgb());
        for (int y = 0; y < 2; ++y)
        {
            for (int x = 0; x < 256; ++x)
            {
                gray.plane(0).row(y)[x] = static_cast<std::uint8_t>(x);
            }
        }
        const Frame expanded = convert(gray, to_srgb24()).frame;
        for (int x = 0; x < 256; ++x)
        {
            for (int channel = 0; channel < 3; ++channel)
            {
                assert(expanded.plane(0).row(1)[x * 3 + channel] == x);
            }
        }

        // A palette lookup is exact; bgr24 takes swscale's own path.
        const Frame palette = decode_image(Source::from_path(data_path("testsrc2_160x144_pal8.png"))).frame;
        const Frame looked_up = convert(palette, to_srgb24()).frame;
        const Frame reference = convert(palette, PixelFormat::from_name("bgr24"), ColorSpec::srgb()).frame;
        for (int y = 0; y < palette.height(); ++y)
        {
            for (int x = 0; x < palette.width(); ++x)
            {
                for (int channel = 0; channel < 3; ++channel)
                {
                    assert(looked_up.plane(0).row(y)[x * 3 + channel] ==
                           reference.plane(0).row(y)[x * 3 + 2 - channel]);
                }
            }
        }
    }

    void test_sixteen_bit_samples_are_rounded_once_without_dither()
    {
        Frame wide = Frame::allocate(40, 24, PixelFormat::from_name("rgb48"), ColorSpec::srgb());
        for (int y = 0; y < wide.height(); ++y)
        {
            auto* row = reinterpret_cast<std::uint16_t*>(wide.plane(0).row(y));
            for (int i = 0; i < wide.width() * 3; ++i)
            {
                row[i] = static_cast<std::uint16_t>((i * 997 + y * 4099) % 65536);
            }
        }
        const Frame rounded = convert(wide, to_srgb24()).frame;
        for (int y = 0; y < wide.height(); ++y)
        {
            const auto* row = reinterpret_cast<const std::uint16_t*>(wide.plane(0).row(y));
            for (int i = 0; i < wide.width() * 3; ++i)
            {
                assert(rounded.plane(0).row(y)[i] == (row[i] * 255U + 32767U) / 65535U);
            }
        }
    }

    void test_alpha_is_composited_over_black_or_discarded()
    {
        Frame rgba = Frame::allocate(16, 16, PixelFormat::from_name("rgba"), ColorSpec::srgb());
        for (int y = 0; y < 16; ++y)
        {
            for (int x = 0; x < 16; ++x)
            {
                std::uint8_t* pixel = rgba.plane(0).row(y) + x * 4;
                pixel[0] = static_cast<std::uint8_t>(x * 16 + 15);
                pixel[1] = static_cast<std::uint8_t>(y * 16);
                pixel[2] = 200;
                pixel[3] = static_cast<std::uint8_t>((x * 16 + y) % 256);
            }
        }

        const FrameResult over_black = convert(rgba, to_srgb24());
        assert(std::get<ConvertEvidence>(over_black.record.evidence).alpha == "over_black");
        assert(has_conversion(over_black.record.conversions, "alpha"));
        ConvertOptions discard = to_srgb24();
        discard.alpha = AlphaHandling::Discard;
        const FrameResult discarded = convert(rgba, discard);
        assert(std::get<ConvertEvidence>(discarded.record.evidence).alpha == "discarded");

        for (int y = 0; y < 16; ++y)
        {
            for (int x = 0; x < 16; ++x)
            {
                const std::uint8_t* pixel = rgba.plane(0).row(y) + x * 4;
                for (int channel = 0; channel < 3; ++channel)
                {
                    const unsigned color = pixel[channel];
                    assert(over_black.frame.plane(0).row(y)[x * 3 + channel] == (color * pixel[3] + 127U) / 255U);
                    assert(discarded.frame.plane(0).row(y)[x * 3 + channel] == color);
                }
            }
        }
    }

    void test_colors_tagged_other_than_srgb_are_converted()
    {
        Frame display_p3 = textured_rgb(32, 8);
        ColorSpec tags = ColorSpec::srgb();
        tags.primaries = ColorPrimaries::Smpte432;
        display_p3.set_color(tags);
        // A neutral gray row: relative colorimetric keeps it neutral.
        for (int x = 0; x < 32; ++x)
        {
            std::uint8_t* pixel = display_p3.plane(0).row(0) + x * 3;
            pixel[0] = pixel[1] = pixel[2] = static_cast<std::uint8_t>(x * 8);
        }

        const FrameResult converted = convert(display_p3, to_srgb24());
        const ConvertEvidence& evidence = std::get<ConvertEvidence>(converted.record.evidence);
        assert(evidence.color_transform == "color_tags");
        assert(!evidence.icc_profile_sha256.has_value());
        assert(has_conversion(converted.record.conversions, "primaries"));
        assert(converted.frame.color() == ColorSpec::srgb());
        assert(plane_difference(converted.frame, display_p3) > 0.0);
        for (int x = 0; x < 32; ++x)
        {
            const std::uint8_t* pixel = converted.frame.plane(0).row(0) + x * 3;
            assert(std::abs(pixel[0] - pixel[1]) <= 1 && std::abs(pixel[1] - pixel[2]) <= 1);
            assert(std::abs(pixel[1] - x * 8) <= 1);
        }

        ConvertOptions ignore = to_srgb24();
        ignore.icc = IccHandling::Ignore;
        try
        {
            (void)convert(display_p3, ignore);
            assert(false && "expected throw");
        }
        catch (const NotImplemented&)
        {
        }
    }

    void test_a_profile_is_applied_once_and_dropped()
    {
        const Frame p3 = decode_image(Source::from_path(data_path("testsrc_64x48_p3.png"))).frame;
        assert(p3.icc_profile() != nullptr);
        const FrameResult converted = convert(p3, to_srgb24());
        const ConvertEvidence& evidence = std::get<ConvertEvidence>(converted.record.evidence);
        assert(evidence.color_transform == "icc_profile");
        assert(evidence.icc_profile_sha256.has_value() && evidence.icc_profile_sha256->starts_with("sha256:"));
        assert(has_conversion(converted.record.conversions, "icc_profile"));
        assert(converted.frame.icc_profile() == nullptr);

        // Converting again finds sRGB and changes nothing.
        const FrameResult again = convert(converted.frame, to_srgb24());
        assert(std::get<ConvertEvidence>(again.record.evidence).color_transform == "none");
        assert(samples_equal(again.frame, converted.frame));
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
        assert(result.record.kind() == StageKind::ChromaRoundtrip);
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
        assert(std::get<ChromaRoundtripEvidence>(result.record.evidence).intermediate_pixel_format.name() ==
               "yuv420p");
        assert(std::get<ChromaRoundtripOptions>(result.configuration).intermediate_bit_depth == 8);
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
        assert(std::holds_alternative<ReinterpretEvidence>(result.record.evidence));
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
        assert(result.record.kind() == StageKind::Reinterpret);
        assert(std::get<ReinterpretOptions>(result.configuration).as_color == result.frame.color());
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
    test_the_chroma_upsampler_asked_for_is_used_at_every_size();
    test_eight_bit_rgb_gray_and_palette_sources_keep_their_values();
    test_sixteen_bit_samples_are_rounded_once_without_dither();
    test_alpha_is_composited_over_black_or_discarded();
    test_colors_tagged_other_than_srgb_are_converted();
    test_a_profile_is_applied_once_and_dropped();
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
