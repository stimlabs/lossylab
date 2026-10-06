#include "lossylab/codec/encode.hpp"
#include "lossylab/convert/convert.hpp"
#include "lossylab/core/error.hpp"
#include "lossylab/io/decode_image.hpp"
#include "lossylab/transform/transform.hpp"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <string>

using namespace lossylab;

namespace
{
    std::string data_path(const std::string_view name)
    {
        return std::string(LOSSYLAB_TEST_DATA_DIR) + "/" + std::string(name);
    }

    DecodedImage decode(const std::string_view name)
    {
        return decode_image(Source::from_path(data_path(name)));
    }

    /// The frame as sRGB rgb24, so frames of any format compare sample by sample.
    Frame as_rgb24(const Frame& frame)
    {
        ConvertOptions options;
        options.pixel_format = PixelFormat::from_name("rgb24");
        options.color = ColorSpec::srgb();
        options.icc = IccHandling::Ignore;
        return convert(frame, options).frame;
    }

    bool same_rgb24(const Frame& left, const Frame& right)
    {
        if (left.width() != right.width() || left.height() != right.height())
        {
            return false;
        }
        for (int y = 0; y < left.height(); ++y)
        {
            if (std::memcmp(left.plane(0).row(y), right.plane(0).row(y), static_cast<std::size_t>(left.width() * 3)) !=
                0)
            {
                return false;
            }
        }
        return true;
    }

    template <typename Exception, typename Function>
    void expect_throw(Function&& function)
    {
        try
        {
            function();
            assert(false && "expected throw");
        }
        catch (const Exception&)
        {
        }
    }

    void test_a_crop_copies_the_samples_exactly()
    {
        for (const char* name : {"testsrc2_160x144_pal8", "testsrc2_160x144_monob"})
        {
            const Frame source = decode(std::string(name) + ".png").frame;
            const FrameResult cropped = crop(source, CropOptions{48, 32, 64, 64});
            assert(cropped.record.kind() == StageKind::Crop);
            assert(cropped.frame.pixel_format() == source.pixel_format());
            assert(cropped.record.transform.translate_x == -48 && cropped.record.transform.translate_y == -32);
            const Frame expected = decode(std::string(name) + "_crop_48_32_64x64.png").frame;
            assert(same_rgb24(as_rgb24(cropped.frame), as_rgb24(expected)));
        }
    }

    void test_a_crop_outside_the_frame_is_refused()
    {
        const Frame source = decode("testsrc_64x48.png").frame;
        expect_throw<ConfigError>([&] { (void)crop(source, CropOptions{40, 0, 32, 16}); });
        expect_throw<ConfigError>([&] { (void)crop(source, CropOptions{0, 0, 0, 16}); });
        expect_throw<ConfigError>([&] { (void)crop(source, CropOptions{-1, 0, 8, 8}); });
    }

    void test_a_decoded_jpeg_records_its_grid_and_a_crop_where_it_falls()
    {
        const DecodedImage jpeg = decode("testsrc_64x48_q75.jpg");
        assert(jpeg.record.block_grid.has_value());
        const BlockGrid grid = *jpeg.record.block_grid;
        assert(grid.kind == BlockGridKind::JpegMcu);
        assert(grid.block_width == 16 && grid.block_height == 16);

        // A 4:2:0 frame can only be cut on whole chroma samples; its RGB
        // rendering anywhere.
        const Frame rgb = as_rgb24(jpeg.frame);
        const CropEvidence aligned =
            std::get<CropEvidence>(crop(rgb, CropOptions{16, 16, 32, 16}, grid).record.evidence);
        assert(aligned.block_grid->phase_x == 0 && aligned.block_grid->phase_y == 0);
        assert(aligned.whole_blocks == true);

        const CropEvidence misaligned =
            std::get<CropEvidence>(crop(rgb, CropOptions{5, 3, 32, 20}, grid).record.evidence);
        assert(misaligned.block_grid->phase_x != 0 && misaligned.block_grid->phase_y != 0);
        assert(misaligned.whole_blocks == false);

        expect_throw<ConfigError>([&] { (void)crop(jpeg.frame, CropOptions{5, 3, 32, 20}); });
        assert(!std::get<CropEvidence>(crop(rgb, CropOptions{0, 0, 8, 8}).record.evidence).block_grid);
    }

    void test_orient_turns_every_orientation_upright()
    {
        for (int orientation = 2; orientation <= 8; ++orientation)
        {
            const std::string stored = "testsrc_64x48_orientation" + std::to_string(orientation);
            const Frame source = decode(stored + ".png").frame;
            assert(source.orientation() == orientation);

            const FrameResult upright = orient(source, OrientOptions{orientation});
            assert(upright.record.kind() == StageKind::Orient);
            assert(std::get<OrientEvidence>(upright.record.evidence).frame_orientation == orientation);
            assert(!upright.frame.orientation().has_value());
            assert(same_rgb24(as_rgb24(upright.frame), as_rgb24(decode(stored + "_upright.png").frame)));
        }
    }

    void test_orientation_one_changes_no_sample_and_others_are_refused()
    {
        const Frame source = decode("testsrc_64x48.png").frame;
        const FrameResult same = orient(source, OrientOptions{1});
        assert(same_rgb24(same.frame, source));
        assert(same.record.transform.is_identity());
        expect_throw<ConfigError>([&] { (void)orient(source, OrientOptions{9}); });
        expect_throw<ConfigError>([&] { (void)orient(source, OrientOptions{0}); });
    }

    void test_achromatic_sets_every_channel_to_the_luma()
    {
        const Frame source = decode("testsrc_64x48.png").frame;
        const FrameResult gray = achromatic(source);
        assert(gray.record.kind() == StageKind::Achromatic);
        for (int y = 0; y < source.height(); ++y)
        {
            const std::uint8_t* input = source.plane(0).row(y);
            const std::uint8_t* output = gray.frame.plane(0).row(y);
            for (int x = 0; x < source.width(); ++x)
            {
                const unsigned luma =
                    (19595U * input[x * 3] + 38470U * input[x * 3 + 1] + 7471U * input[x * 3 + 2] + 32768U) >> 16;
                assert(output[x * 3] == luma && output[x * 3 + 1] == luma && output[x * 3 + 2] == luma);
            }
        }
        const AchromaticEvidence& colored = std::get<AchromaticEvidence>(gray.record.evidence);
        assert(colored.channel_spread_max > 0 && colored.channel_spread_mean > 0.0);

        const AchromaticEvidence already = std::get<AchromaticEvidence>(achromatic(gray.frame).record.evidence);
        assert(already.channel_spread_max == 0 && already.channel_spread_mean == 0.0 &&
               already.channel_spread_std == 0.0);

        expect_throw<ConfigError>([&] { (void)achromatic(decode("testsrc_64x48_q75.jpg").frame); });
    }

    void test_an_achromatic_frame_survives_a_jpeg_round_trip_gray()
    {
        ColorSpec jpeg_color = ColorSpec::jpeg();
        jpeg_color.primaries = ColorPrimaries::Bt709;
        jpeg_color.transfer = TransferCharacteristic::Srgb;

        EncodeImageOptions encode;
        encode.codec = ImageCodec::Mjpeg;
        encode.pixel_format = PixelFormat::from_name("yuvj420p");
        encode.color = jpeg_color;
        encode.rate_control = RateControl::quality(2);
        encode.strict = Strict::AllowRecorded;

        DecodeSpec decode_spec;
        decode_spec.conversion = ConvertOptions{};
        decode_spec.conversion->pixel_format = PixelFormat::from_name("rgb24");
        decode_spec.conversion->color = ColorSpec::srgb();

        const Frame gray = achromatic(decode("testsrc_64x48.png").frame).frame;
        const Frame decoded = roundtrip(gray, encode, decode_spec).frame;
        for (int y = 0; y < decoded.height(); ++y)
        {
            const std::uint8_t* row = decoded.plane(0).row(y);
            for (int x = 0; x < decoded.width(); ++x)
            {
                assert(row[x * 3] == row[x * 3 + 1] && row[x * 3 + 1] == row[x * 3 + 2]);
            }
        }
    }
}

int main()
{
    test_a_crop_copies_the_samples_exactly();
    test_a_crop_outside_the_frame_is_refused();
    test_a_decoded_jpeg_records_its_grid_and_a_crop_where_it_falls();
    test_orient_turns_every_orientation_upright();
    test_orientation_one_changes_no_sample_and_others_are_refused();
    test_achromatic_sets_every_channel_to_the_luma();
    test_an_achromatic_frame_survives_a_jpeg_round_trip_gray();
}
