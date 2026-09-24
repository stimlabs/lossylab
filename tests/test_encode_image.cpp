#include "lossylab/codec/encode.hpp"
#include "lossylab/convert/convert.hpp"
#include "lossylab/core/error.hpp"
#include "lossylab/env/capabilities.hpp"
#include "lossylab/io/decode_image.hpp"
#include "lossylab/io/probe.hpp"
#include "lossylab/measure/measure.hpp"

#include <cassert>
#include <cmath>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

using namespace lossylab;

namespace
{
    std::string data_path(const std::string_view name)
    {
        return std::string(LOSSYLAB_TEST_DATA_DIR) + "/" + std::string(name);
    }

    /// The sRGB test image, RGB as stored.
    Frame rgb_source()
    {
        return decode_image(Source::from_path(data_path("testsrc_64x48.png"))).frame;
    }

    /// BT.601 YUV that keeps the source's primaries and transfer, since
    /// convert() changes neither.
    ColorSpec bt601_yuv(const ColorRange range)
    {
        ColorSpec color = ColorSpec::srgb();
        color.matrix = ColorMatrix::Bt470bg;
        color.range = range;
        color.chroma_location = ChromaLocation::Center;
        return color;
    }

    Frame source_in(const char* pixel_format, const ColorSpec& color)
    {
        return convert(rgb_source(), PixelFormat::from_name(pixel_format), color).frame;
    }

    EncodeImageOptions image_options(const ImageCodec codec, const char* pixel_format, const double quality)
    {
        EncodeImageOptions options;
        options.codec = codec;
        options.pixel_format = PixelFormat::from_name(pixel_format);
        options.rate_control = RateControl::quality(quality);
        return options;
    }

    bool starts_with(const std::vector<std::uint8_t>& bytes, const std::size_t offset, const std::string_view text)
    {
        return bytes.size() >= offset + text.size() &&
               std::memcmp(bytes.data() + offset, text.data(), text.size()) == 0;
    }

    double psnr_of(const Frame& reference, const Frame& distorted)
    {
        CompareOptions options;
        options.strict = Strict::AllowRecorded;
        return compare(reference, distorted, {Metric::Psnr}, options).frames.front().at("psnr");
    }

    template <typename Exception, typename Callable>
    void expect_throw(Callable&& callable)
    {
        try
        {
            callable();
            assert(false && "expected an exception");
        }
        catch (const Exception&)
        {
        }
    }

    void test_mjpeg_encodes_at_a_fixed_qscale()
    {
        const Frame source = source_in("yuvj420p", bt601_yuv(ColorRange::Full));
        const EncodedResult encoded = encode_image(source, image_options(ImageCodec::Mjpeg, "yuvj420p", 5));

        assert(starts_with(encoded.bytes, 0, "\xFF\xD8"));
        const StageRecord& record = encoded.record;
        assert(record.kind == StageKind::EncodeImage);
        assert(record.implementation == "mjpeg");
        assert(record.frames.size() == 1);
        assert(record.frames.front().qp_mean == 5.0);
        assert(record.frames.front().picture_type == PictureType::I);
        assert(record.frames.front().size_bytes == static_cast<std::int64_t>(encoded.bytes.size()));
        assert(record.block_grid.has_value() && record.block_grid->kind == BlockGridKind::Dct8);
        assert(record.encoder_settings.at("resolved").at("fixed_qscale").get<int>() == 5);
        assert(record.encoder_settings.at("resolved").at("bitexact").get<bool>());
        assert(record.achieved_bpp.has_value() && std::abs(*record.achieved_bpp - encoded.bits_per_pixel()) < 1e-9);
        assert(record.conversions.empty());

        // FFmpeg accounts for part of the stage's time, never more than all of it.
        assert(record.ffmpeg_duration_ms > 0.0);
        assert(record.ffmpeg_duration_ms <= record.duration_ms);

        // The file decodes back with the size and chroma layout it went in with.
        const Frame decoded = decode_image(Source::from_memory(encoded.bytes)).frame;
        assert(decoded.width() == 64 && decoded.height() == 48);
        assert(decoded.pixel_format().name() == "yuvj420p");
    }

    void test_a_coarser_qscale_makes_a_smaller_file()
    {
        const Frame source = source_in("yuvj420p", bt601_yuv(ColorRange::Full));
        const auto fine = encode_image(source, image_options(ImageCodec::Mjpeg, "yuvj420p", 2)).bytes.size();
        const auto coarse = encode_image(source, image_options(ImageCodec::Mjpeg, "yuvj420p", 20)).bytes.size();
        assert(coarse < fine);
    }

    void test_encoding_is_deterministic()
    {
        const Frame source = source_in("yuvj420p", bt601_yuv(ColorRange::Full));
        const EncodeImageOptions options = image_options(ImageCodec::Mjpeg, "yuvj420p", 4);
        assert(encode_image(source, options).bytes == encode_image(source, options).bytes);
    }

    void test_mjpeg_refuses_what_jpeg_cannot_carry()
    {
        const Frame limited = source_in("yuv420p", bt601_yuv(ColorRange::Limited));
        expect_throw<ConfigError>([&] { (void)encode_image(limited, image_options(ImageCodec::Mjpeg, "yuv420p", 5)); });

        const Frame full = source_in("yuvj420p", bt601_yuv(ColorRange::Full));
        expect_throw<ConfigError>([&] { (void)encode_image(full, image_options(ImageCodec::Mjpeg, "yuvj420p", 0)); });
        expect_throw<ConfigError>([&] { (void)encode_image(full, image_options(ImageCodec::Mjpeg, "yuvj420p", 2.5)); });

        EncodeImageOptions crf = image_options(ImageCodec::Mjpeg, "yuvj420p", 5);
        crf.rate_control = RateControl::crf(20);
        expect_throw<ConfigError>([&] { (void)encode_image(full, crf); });
    }

    void test_a_conversion_to_the_encoders_format_is_refused_or_recorded()
    {
        const Frame rgb = rgb_source();
        EncodeImageOptions options = image_options(ImageCodec::Mjpeg, "yuvj420p", 5);
        options.color = bt601_yuv(ColorRange::Full);
        expect_throw<ConversionRefused>([&] { (void)encode_image(rgb, options); });

        options.strict = Strict::AllowRecorded;
        const EncodedResult encoded = encode_image(rgb, options);
        assert(!encoded.record.conversions.empty());
        assert(encoded.record.conversions.front().cause == ConversionCause::CodecConstraint);
        assert(encoded.record.input.pixel_format.name() == "rgb24");
        assert(encoded.record.output.pixel_format.name() == "yuvj420p");

        // Without a color to encode in, RGB to YUV is not a guess the library makes.
        options.color.reset();
        expect_throw<ConfigError>([&] { (void)encode_image(rgb, options); });
    }

    void test_an_option_set_from_the_rate_control_cannot_be_overridden()
    {
        if (!capabilities().supports(ImageCodec::WebP))
        {
            return;
        }
        EncodeImageOptions options = image_options(ImageCodec::WebP, "yuv420p", 75);
        options.encoder_options = {{"quality", "10"}};
        expect_throw<ConfigError>(
            [&] { (void)encode_image(source_in("yuv420p", bt601_yuv(ColorRange::Limited)), options); });
    }

    void test_lossy_webp_roundtrips_through_its_decoder()
    {
        if (!capabilities().supports(ImageCodec::WebP))
        {
            return;
        }
        const Frame source = source_in("yuv420p", bt601_yuv(ColorRange::Limited));
        const EncodeImageOptions options = image_options(ImageCodec::WebP, "yuv420p", 80);
        const EncodedResult encoded = encode_image(source, options);
        assert(starts_with(encoded.bytes, 0, "RIFF") && starts_with(encoded.bytes, 8, "WEBP"));
        assert(encoded.record.block_grid->kind == BlockGridKind::Macroblock16);

        DecodeSpec decode_spec;
        decode_spec.pixel_format = source.pixel_format();
        const FrameResult result = roundtrip(source, options, decode_spec);
        assert(result.record.kind == StageKind::Roundtrip);
        assert(result.record.params.contains("encode") && result.record.params.contains("decode"));
        assert(result.frame.describe() == source.describe());
        const double psnr = psnr_of(source, result.frame);
        assert(psnr > 25.0 && std::isfinite(psnr));

        // bgra would go through libwebp's own RGB to YUV conversion.
        const Frame bgra = source_in("bgra", ColorSpec::srgb());
        expect_throw<ConfigError>([&] { (void)encode_image(bgra, image_options(ImageCodec::WebP, "bgra", 80)); });
    }

    void test_lossless_formats_roundtrip_exactly()
    {
        struct Case
        {
            ImageCodec codec;
            const char* pixel_format;
        };
        for (const Case& lossless : {Case{ImageCodec::Png, "rgb24"}, Case{ImageCodec::WebP, "bgra"},
                                     Case{ImageCodec::Jxl, "rgb24"}, Case{ImageCodec::Jpeg2000, "rgb24"}})
        {
            if (!capabilities().supports(lossless.codec))
            {
                continue;
            }
            const Frame source = source_in(lossless.pixel_format, ColorSpec::srgb());
            EncodeImageOptions options = image_options(lossless.codec, lossless.pixel_format, 50);
            options.lossless = true;

            DecodeSpec decode_spec;
            decode_spec.pixel_format = source.pixel_format();
            const FrameResult result = roundtrip(source, options, decode_spec);
            assert(result.frame.describe() == source.describe());
            assert(std::isinf(psnr_of(source, result.frame)));
            assert(!result.record.block_grid.has_value());
        }

        EncodeImageOptions lossy_png = image_options(ImageCodec::Png, "rgb24", 50);
        expect_throw<ConfigError>([&] { (void)encode_image(rgb_source(), lossy_png); });
    }

    void test_lossy_jpeg_xl_encodes_at_a_distance()
    {
        if (!capabilities().supports(ImageCodec::Jxl))
        {
            return;
        }
        const Frame source = rgb_source();
        const EncodeImageOptions options = image_options(ImageCodec::Jxl, "rgb24", 1.0);
        const EncodedResult encoded = encode_image(source, options);
        assert(encoded.record.encoder_settings.at("resolved").at("options").at("distance").get<std::string>() ==
               "1.000000");

        DecodeSpec decode_spec;
        decode_spec.pixel_format = source.pixel_format();
        // FFmpeg's CLI measures the same 24.6 dB for this encode of the sharp,
        // tiny test pattern.
        const double psnr = psnr_of(source, roundtrip(source, options, decode_spec).frame);
        assert(psnr > 20.0 && std::isfinite(psnr));
    }

    void test_lossy_jpeg_2000_encodes_at_a_compression_ratio()
    {
        if (!capabilities().supports(ImageCodec::Jpeg2000))
        {
            return;
        }
        const Frame source = rgb_source();
        const EncodeImageOptions options = image_options(ImageCodec::Jpeg2000, "rgb24", 8);
        const EncodedResult encoded = encode_image(source, options);
        assert(starts_with(encoded.bytes, 4, "jP  "));
        assert(encoded.record.encoder_settings.at("resolved").at("options").at("layer_rates").get<std::string>() ==
               "8");
        assert(encoded.record.encoder_settings.at("quality_scale").get<std::string>().starts_with("a nominal"));

        const auto coarser = encode_image(source, image_options(ImageCodec::Jpeg2000, "rgb24", 32)).bytes.size();
        assert(coarser < encoded.bytes.size());

        DecodeSpec decode_spec;
        decode_spec.pixel_format = source.pixel_format();
        const double psnr = psnr_of(source, roundtrip(source, options, decode_spec).frame);
        assert(psnr > 25.0 && std::isfinite(psnr));

        expect_throw<ConfigError>([&] { (void)encode_image(source, image_options(ImageCodec::Jpeg2000, "rgb24", 0)); });
        expect_throw<ConfigError>(
            [&] { (void)encode_image(source, image_options(ImageCodec::Jpeg2000, "rgb24", 8.5)); });
        EncodeImageOptions crf = options;
        crf.rate_control = RateControl::crf(20);
        expect_throw<ConfigError>([&] { (void)encode_image(source, crf); });
    }

    void test_avif_is_muxed_into_an_avif_file()
    {
        if (!capabilities().supports(ImageCodec::Avif))
        {
            return;
        }
        const Frame source = source_in("yuv420p", bt601_yuv(ColorRange::Limited));
        const EncodeImageOptions options = image_options(ImageCodec::Avif, "yuv420p", 30);
        const EncodedResult encoded = encode_image(source, options);
        assert(starts_with(encoded.bytes, 4, "ftypavif"));
        assert(encoded.record.params.at("container").get<std::string>() == "avif");

        const ProbeResult probed = probe(Source::from_memory(encoded.bytes));
        assert(probed.streams.front().width == 64 && probed.streams.front().height == 48);

        DecodeSpec decode_spec;
        decode_spec.pixel_format = source.pixel_format();
        const FrameResult result = roundtrip(source, options, decode_spec);
        assert(result.frame.describe() == source.describe());
        assert(psnr_of(source, result.frame) > 25.0);
    }

    void test_heif_cannot_be_encoded()
    {
        const Frame source = source_in("yuv420p", bt601_yuv(ColorRange::Limited));
        expect_throw<UnsupportedCapability>(
            [&] { (void)encode_image(source, image_options(ImageCodec::Heif, "yuv420p", 30)); });
    }
}

int main()
{
    test_mjpeg_encodes_at_a_fixed_qscale();
    test_a_coarser_qscale_makes_a_smaller_file();
    test_encoding_is_deterministic();
    test_mjpeg_refuses_what_jpeg_cannot_carry();
    test_a_conversion_to_the_encoders_format_is_refused_or_recorded();
    test_an_option_set_from_the_rate_control_cannot_be_overridden();
    test_lossy_webp_roundtrips_through_its_decoder();
    test_lossless_formats_roundtrip_exactly();
    test_lossy_jpeg_xl_encodes_at_a_distance();
    test_lossy_jpeg_2000_encodes_at_a_compression_ratio();
    test_avif_is_muxed_into_an_avif_file();
    test_heif_cannot_be_encoded();
    return 0;
}
