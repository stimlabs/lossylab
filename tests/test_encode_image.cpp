#include "lossylab/codec/encode.hpp"
#include "lossylab/convert/convert.hpp"
#include "lossylab/core/error.hpp"
#include "lossylab/env/capabilities.hpp"
#include "lossylab/io/decode_image.hpp"
#include "lossylab/io/probe.hpp"
#include "lossylab/measure/measure.hpp"

#include <array>
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

    /// Decodes back to the source's own format and color.
    DecodeSpec decode_like(const Frame& source)
    {
        ConvertOptions conversion;
        conversion.pixel_format = source.pixel_format();
        conversion.color = source.color();
        DecodeSpec decode_spec;
        decode_spec.conversion = conversion;
        return decode_spec;
    }

    bool starts_with(const std::vector<std::uint8_t>& bytes, const std::size_t offset, const std::string_view text)
    {
        return bytes.size() >= offset + text.size() &&
               std::memcmp(bytes.data() + offset, text.data(), text.size()) == 0;
    }

    double psnr_of(const Frame& reference, const Frame& distorted)
    {
        CompareOptions options;
        options.metrics = {Metric::Psnr};
        options.strict = Strict::AllowRecorded;
        return compare(reference, distorted, options).evidence().frames.front().at("psnr");
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
        assert(record.kind() == StageKind::EncodeImage);
        assert(record.implementation == "mjpeg");
        assert(record.frames.size() == 1);
        assert(record.frames.front().qp_mean == 5.0);
        assert(record.frames.front().picture_type == PictureType::I);
        assert(record.frames.front().size_bytes == static_cast<std::int64_t>(encoded.bytes.size()));
        assert(record.block_grid.has_value() && record.block_grid->kind == BlockGridKind::Dct8);
        const EncodeImageEvidence& evidence = std::get<EncodeImageEvidence>(record.evidence);
        assert(evidence.resolved.fixed_qscale == 5);
        assert(evidence.resolved.bitexact);
        assert(std::abs(evidence.achieved_bpp - encoded.bits_per_pixel()) < 1e-9);
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
        for (const EncodeImageOptions& options :
             {image_options(ImageCodec::Mjpeg, "yuvj420p", 4), image_options(ImageCodec::Jpeg, "yuvj420p", 75)})
        {
            assert(encode_image(source, options).bytes == encode_image(source, options).bytes);
        }
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

    /// The file's JPEG markers, as probe() reads them.
    JpegInfo jpeg_info(const std::vector<std::uint8_t>& bytes)
    {
        return *probe(Source::from_memory(bytes, "jpg")).streams.front().jpeg;
    }

    struct JpegLayout
    {
        const char* pixel_format;
        int horizontal_sampling;
        int vertical_sampling;
    };

    constexpr std::array<JpegLayout, 4> jpeg_layouts{JpegLayout{"yuvj420p", 2, 2}, JpegLayout{"yuvj422p", 2, 1},
                                                     JpegLayout{"yuvj440p", 1, 2}, JpegLayout{"yuvj444p", 1, 1}};

    void test_jpeg_encodes_with_libjpegs_tables_at_an_ijg_quality()
    {
        for (const JpegLayout& layout : jpeg_layouts)
        {
            const Frame source = source_in(layout.pixel_format, bt601_yuv(ColorRange::Full));
            for (const int quality : {1, 2, 25, 50, 75, 90, 99, 100})
            {
                const EncodedResult encoded =
                    encode_image(source, image_options(ImageCodec::Jpeg, layout.pixel_format, quality));
                assert(starts_with(encoded.bytes, 0, "\xFF\xD8"));

                const JpegInfo info = jpeg_info(encoded.bytes);
                assert(info.ijg_quality == quality && info.ijg_quality_exact);
                assert(info.process == "baseline" && info.huffman_tables == "standard" && info.scan_count == 1);
                assert(info.components.size() == 3);
                assert(info.components[0].horizontal_sampling == layout.horizontal_sampling);
                assert(info.components[0].vertical_sampling == layout.vertical_sampling);
                for (std::size_t index = 1; index < 3; ++index)
                {
                    assert(info.components[index].horizontal_sampling == 1);
                    assert(info.components[index].vertical_sampling == 1);
                }

                const Frame decoded = decode_image(Source::from_memory(encoded.bytes, "jpg")).frame;
                assert(decoded.width() == 64 && decoded.height() == 48);
                assert(decoded.pixel_format().name() == layout.pixel_format);
            }
        }
    }

    void test_jpeg_records_libjpeg_and_its_settings()
    {
        const Frame source = source_in("yuvj420p", bt601_yuv(ColorRange::Full));
        const EncodedResult encoded = encode_image(source, image_options(ImageCodec::Jpeg, "yuvj420p", 80));

        const StageRecord& record = encoded.record;
        assert(record.kind() == StageKind::EncodeImage);
        assert(record.implementation == "libjpeg-turbo");
        assert(record.conversions.empty());
        assert(record.frames.size() == 1);
        assert(record.frames.front().picture_type == PictureType::I && record.frames.front().key_frame);
        assert(record.frames.front().size_bytes == static_cast<std::int64_t>(encoded.bytes.size()));
        assert(!record.frames.front().qp_mean.has_value());
        assert(record.block_grid.has_value() && record.block_grid->kind == BlockGridKind::Dct8);

        const EncodeImageEvidence& evidence = std::get<EncodeImageEvidence>(record.evidence);
        assert(evidence.extension == "jpg" && !evidence.container.has_value());
        assert(!evidence.resolved.fixed_qscale.has_value());
        assert(evidence.resolved.options.at("quality") == "80");
        assert(evidence.resolved.options.at("dct_method") == "islow");
        assert(evidence.resolved.options.at("optimize_coding") == "0");
        assert(evidence.resolved.options.at("progressive") == "0");
        assert(evidence.resolved.options.at("sampling_factors") == "2x2,1x1,1x1");
        assert(std::abs(evidence.achieved_bpp - encoded.bits_per_pixel()) < 1e-9);

        const auto coarser = encode_image(source, image_options(ImageCodec::Jpeg, "yuvj420p", 30)).bytes.size();
        assert(coarser < encoded.bytes.size());
    }

    void test_jpeg_fills_out_partial_blocks()
    {
        // 61x45 ends in partial blocks and MCUs on the right and at the bottom, in every layout.
        for (const JpegLayout& layout : jpeg_layouts)
        {
            Frame source = Frame::allocate(61, 45, PixelFormat::from_name(layout.pixel_format),
                                           bt601_yuv(ColorRange::Full));
            for (int index = 0; index < source.plane_count(); ++index)
            {
                const PlaneView plane = source.plane(index);
                for (int y = 0; y < plane.height; ++y)
                {
                    for (int x = 0; x < plane.width; ++x)
                    {
                        plane.row(y)[x] = static_cast<std::uint8_t>(64 + 2 * x + y + 16 * index);
                    }
                }
            }

            const EncodeImageOptions options = image_options(ImageCodec::Jpeg, layout.pixel_format, 95);
            const FrameResult result = roundtrip(source, options, decode_like(source));
            assert(result.frame.width() == 61 && result.frame.height() == 45);
            assert(psnr_of(source, result.frame) > 40.0);
        }
    }

    void test_jpeg_refuses_what_it_cannot_take()
    {
        const Frame full = source_in("yuvj420p", bt601_yuv(ColorRange::Full));
        for (const double quality : {0.0, 101.0, 74.5})
        {
            expect_throw<ConfigError>(
                [&] { (void)encode_image(full, image_options(ImageCodec::Jpeg, "yuvj420p", quality)); });
        }

        EncodeImageOptions crf = image_options(ImageCodec::Jpeg, "yuvj420p", 75);
        crf.rate_control = RateControl::crf(20);
        expect_throw<ConfigError>([&] { (void)encode_image(full, crf); });

        EncodeImageOptions lossless = image_options(ImageCodec::Jpeg, "yuvj420p", 75);
        lossless.lossless = true;
        expect_throw<ConfigError>([&] { (void)encode_image(full, lossless); });

        EncodeImageOptions with_options = image_options(ImageCodec::Jpeg, "yuvj420p", 75);
        with_options.encoder_options = {{"optimize_coding", "1"}};
        expect_throw<ConfigError>([&] { (void)encode_image(full, with_options); });

        const Frame limited = source_in("yuv420p", bt601_yuv(ColorRange::Limited));
        expect_throw<ConfigError>([&] { (void)encode_image(limited, image_options(ImageCodec::Jpeg, "yuv420p", 75)); });

        // RGB and gray would go through libjpeg's own color handling.
        const Frame rgb = rgb_source();
        expect_throw<ConfigError>([&] { (void)encode_image(rgb, image_options(ImageCodec::Jpeg, "rgb24", 75)); });
        const Frame gray = source_in("gray", bt601_yuv(ColorRange::Full));
        expect_throw<ConfigError>([&] { (void)encode_image(gray, image_options(ImageCodec::Jpeg, "gray", 75)); });

        // Converting RGB to YCbCr first is the library's own conversion, refused or recorded as usual.
        EncodeImageOptions from_rgb = image_options(ImageCodec::Jpeg, "yuvj420p", 75);
        from_rgb.color = bt601_yuv(ColorRange::Full);
        expect_throw<ConversionRefused>([&] { (void)encode_image(rgb, from_rgb); });
        from_rgb.strict = Strict::AllowRecorded;
        const EncodedResult converted = encode_image(rgb, from_rgb);
        assert(!converted.record.conversions.empty());
        assert(converted.record.conversions.front().cause == ConversionCause::CodecConstraint);
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

        const DecodeSpec decode_spec = decode_like(source);
        const FrameResult result = roundtrip(source, options, decode_spec);
        assert(result.record.kind() == StageKind::RoundtripImage);
        const RoundtripImageEvidence& evidence = std::get<RoundtripImageEvidence>(result.record.evidence);
        assert(evidence.encode.extension == "webp" && !evidence.decode.source_sha256.empty());
        const RoundtripImageConfiguration& configuration = std::get<RoundtripImageConfiguration>(result.configuration);
        assert(configuration.encode.codec == options.codec);
        assert(configuration.decode.conversion->pixel_format == source.pixel_format());
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

            const DecodeSpec decode_spec = decode_like(source);
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
        assert(std::get<EncodeImageEvidence>(encoded.record.evidence).resolved.options.at("distance") == "1.000000");

        const DecodeSpec decode_spec = decode_like(source);
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
        assert(std::get<EncodeImageEvidence>(encoded.record.evidence).resolved.options.at("layer_rates") == "8");

        const auto coarser = encode_image(source, image_options(ImageCodec::Jpeg2000, "rgb24", 32)).bytes.size();
        assert(coarser < encoded.bytes.size());

        const DecodeSpec decode_spec = decode_like(source);
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
        assert(std::get<EncodeImageEvidence>(encoded.record.evidence).container == "avif");

        const ProbeResult probed = probe(Source::from_memory(encoded.bytes));
        assert(probed.streams.front().width == 64 && probed.streams.front().height == 48);

        const DecodeSpec decode_spec = decode_like(source);
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

    // ---- ICC profile, orientation and pixel shape --------------------------

    /// The Display P3 profile of a test JPEG.
    std::vector<std::uint8_t> display_p3_profile()
    {
        return decode_image(Source::from_path(data_path("testsrc_64x48_p3_orientation6.jpg"))).frame.icc_profile()->bytes;
    }

    /// `frame` with a Display P3 profile, orientation 6 and 4:3 pixels.
    Frame with_embedded(Frame frame)
    {
        frame.set_icc_profile(display_p3_profile());
        frame.set_orientation(6);
        frame.set_sample_aspect_ratio(Rational{4, 3});
        return frame;
    }

    bool records(const StageRecord& record, const std::string& property)
    {
        for (const ConversionEvent& event : record.conversions)
        {
            if (event.property == property)
            {
                return true;
            }
        }
        return false;
    }

    struct EmbeddedCase
    {
        ImageCodec codec;
        const char* pixel_format;
        ColorSpec color;
        double quality;
        bool lossless;
    };

    std::vector<EmbeddedCase> embedded_cases()
    {
        ColorSpec jpeg_color = bt601_yuv(ColorRange::Full);
        return {
            {ImageCodec::Png, "rgb24", ColorSpec::srgb(), 50, true},
            {ImageCodec::Jpeg, "yuvj420p", jpeg_color, 75, false},
            {ImageCodec::Mjpeg, "yuvj420p", jpeg_color, 5, false},
            {ImageCodec::WebP, "yuv420p", bt601_yuv(ColorRange::Limited), 80, false},
            {ImageCodec::Jxl, "rgb24", ColorSpec::srgb(), 1.0, false},
            {ImageCodec::Avif, "yuv420p", bt601_yuv(ColorRange::Limited), 30, false},
            {ImageCodec::Jpeg2000, "rgb24", ColorSpec::srgb(), 8, false},
        };
    }

    void test_the_record_states_what_each_output_kept()
    {
        for (const EmbeddedCase& embedded_case : embedded_cases())
        {
            if (!capabilities().supports(embedded_case.codec))
            {
                continue;
            }
            const Frame source = with_embedded(source_in(embedded_case.pixel_format, embedded_case.color));
            EncodeImageOptions options = image_options(embedded_case.codec, embedded_case.pixel_format,
                                                       embedded_case.quality);
            options.lossless = embedded_case.lossless;
            options.strict = Strict::AllowRecorded;
            const EncodedResult encoded = encode_image(source, options);

            // What reading the output back finds is what the record says it kept.
            const DecodedImage read_back = decode_image(
                Source::from_memory(encoded.bytes, std::get<EncodeImageEvidence>(encoded.record.evidence).extension));
            const TileGrid* grid = read_back.tile_grid();
            const bool kept_profile = grid != nullptr ? grid->icc_profile.has_value()
                                                      : read_back.stream().icc_profile.has_value();
            const std::optional<int> kept_orientation =
                grid != nullptr ? grid->orientation : read_back.stream().orientation;
            const Rational kept_ratio = read_back.stream().sample_aspect_ratio;

            assert(records(encoded.record, "icc_profile") == !kept_profile);
            assert(records(encoded.record, "orientation") == (kept_orientation != 6));
            assert(records(encoded.record, "sample_aspect_ratio") == (kept_ratio != Rational{4, 3}));
            assert(encoded.record.input.icc_profile == "Display P3");
            assert(encoded.record.output.icc_profile == (kept_profile ? "Display P3" : ""));
        }
    }

    void test_jpeg_keeps_the_profile_and_the_pixel_shape()
    {
        const Frame source = with_embedded(source_in("yuvj420p", bt601_yuv(ColorRange::Full)));
        EncodeImageOptions options = image_options(ImageCodec::Jpeg, "yuvj420p", 75);
        options.strict = Strict::AllowRecorded;
        const EncodedResult encoded = encode_image(source, options);
        assert(!records(encoded.record, "icc_profile"));
        assert(!records(encoded.record, "sample_aspect_ratio"));
        assert(records(encoded.record, "orientation"));

        const ProbeResult written = probe(Source::from_memory(encoded.bytes, "jpg"));
        assert(written.streams.front().icc_profile.has_value());
        assert(written.streams.front().sample_aspect_ratio == (Rational{4, 3}));

        // The orientation it cannot keep is refused unless dropping it is allowed.
        options.strict = Strict::Refuse;
        expect_throw<ConversionRefused>([&] { (void)encode_image(source, options); });
    }

    void test_an_output_that_cannot_keep_the_profile_is_refused()
    {
        if (!capabilities().supports(ImageCodec::WebP))
        {
            return;
        }
        Frame source = source_in("yuv420p", bt601_yuv(ColorRange::Limited));
        source.set_icc_profile(display_p3_profile());
        const EncodeImageOptions options = image_options(ImageCodec::WebP, "yuv420p", 80);
        expect_throw<ConversionRefused>([&] { (void)encode_image(source, options); });

        // Clearing it first is the explicit way to drop it.
        source.clear_icc_profile();
        const EncodedResult encoded = encode_image(source, options);
        assert(!records(encoded.record, "icc_profile"));
    }

    void test_a_frame_without_embedded_data_records_nothing()
    {
        const Frame source = source_in("rgb24", ColorSpec::srgb());
        EncodeImageOptions options = image_options(ImageCodec::Png, "rgb24", 50);
        options.lossless = true;
        const EncodedResult encoded = encode_image(source, options);
        assert(encoded.record.conversions.empty());
        assert(encoded.record.output == source.describe());
    }

    void test_a_non_square_pixel_turns_with_the_image()
    {
        Frame source = source_in("rgb24", ColorSpec::srgb());
        source.set_orientation(6);
        source.set_sample_aspect_ratio(Rational{4, 3});
        EncodeImageOptions options = image_options(ImageCodec::Png, "rgb24", 50);
        options.lossless = true;
        options.strict = Strict::AllowRecorded;
        const EncodedResult encoded = encode_image(source, options);
        if (records(encoded.record, "orientation") || records(encoded.record, "sample_aspect_ratio"))
        {
            return;  // this build's PNG encoder keeps neither, so there is nothing to turn
        }
        DecodeImageOptions decode_options;
        decode_options.orientation = OrientationHandling::Apply;
        const DecodedImage upright = decode_image(Source::from_memory(encoded.bytes, "png"), decode_options);
        assert(upright.frame.width() == 48 && upright.frame.height() == 64);
        assert(upright.frame.sample_aspect_ratio() == (Rational{3, 4}));
        assert(!upright.frame.orientation().has_value());
    }
}

int main()
{
    test_mjpeg_encodes_at_a_fixed_qscale();
    test_a_coarser_qscale_makes_a_smaller_file();
    test_encoding_is_deterministic();
    test_mjpeg_refuses_what_jpeg_cannot_carry();
    test_jpeg_encodes_with_libjpegs_tables_at_an_ijg_quality();
    test_jpeg_records_libjpeg_and_its_settings();
    test_jpeg_fills_out_partial_blocks();
    test_jpeg_refuses_what_it_cannot_take();
    test_a_conversion_to_the_encoders_format_is_refused_or_recorded();
    test_an_option_set_from_the_rate_control_cannot_be_overridden();
    test_lossy_webp_roundtrips_through_its_decoder();
    test_lossless_formats_roundtrip_exactly();
    test_lossy_jpeg_xl_encodes_at_a_distance();
    test_lossy_jpeg_2000_encodes_at_a_compression_ratio();
    test_avif_is_muxed_into_an_avif_file();
    test_heif_cannot_be_encoded();
    test_the_record_states_what_each_output_kept();
    test_jpeg_keeps_the_profile_and_the_pixel_shape();
    test_an_output_that_cannot_keep_the_profile_is_refused();
    test_a_frame_without_embedded_data_records_nothing();
    test_a_non_square_pixel_turns_with_the_image();
    return 0;
}
