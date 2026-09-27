#include "lossylab/codec/encode.hpp"
#include "lossylab/convert/convert.hpp"
#include "lossylab/core/error.hpp"
#include "lossylab/env/capabilities.hpp"
#include "lossylab/io/decode_image.hpp"
#include "lossylab/measure/measure.hpp"

#include <cassert>
#include <map>
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

    ColorSpec bt601_yuv(const ColorRange range)
    {
        ColorSpec color = ColorSpec::srgb();
        color.matrix = ColorMatrix::Bt470bg;
        color.range = range;
        color.chroma_location = ChromaLocation::Center;
        return color;
    }

    /// The test pattern in a JPEG encoder's format, never compressed.
    Frame pristine()
    {
        const Frame rgb = decode_image(Source::from_path(data_path("testsrc_64x48.png"))).frame;
        return convert(rgb, PixelFormat::from_name("yuvj420p"), bt601_yuv(ColorRange::Full)).frame;
    }

    /// The pristine frame after one MJPEG generation at `qscale`, decoded.
    Frame compressed_once(const int qscale)
    {
        EncodeImageOptions options;
        options.codec = ImageCodec::Mjpeg;
        options.pixel_format = PixelFormat::from_name("yuvj420p");
        options.rate_control = RateControl::quality(qscale);
        return roundtrip(pristine(), options).frame;
    }

    RecompressionOptions mjpeg_sweep()
    {
        RecompressionOptions options;
        options.codec = ImageCodec::Mjpeg;
        for (int qscale = 2; qscale <= 14; ++qscale)
        {
            options.parameter_range.push_back(qscale);
        }
        return options;
    }

    void test_a_jpeg_ghost_reveals_the_prior_qscale()
    {
        for (const int prior : {4, 6, 9})
        {
            const RecompressionCurve curve = recompression_curve(compressed_once(prior), mjpeg_sweep());
            assert(curve.evidence().estimated_prior_parameter == static_cast<double>(prior));
            assert(curve.evidence().confidence >= 0.5);
        }
    }

    void test_a_never_compressed_frame_shows_no_ghost()
    {
        const RecompressionCurve curve = recompression_curve(pristine(), mjpeg_sweep());
        assert(!curve.evidence().estimated_prior_parameter.has_value());
        assert(curve.evidence().confidence < 0.5);

        // Without a prior, re-encoding more coarsely always changes more.
        for (std::size_t i = 1; i < curve.evidence().points.size(); ++i)
        {
            assert(curve.evidence().points[i].error > curve.evidence().points[i - 1].error);
            assert(curve.evidence().points[i].bits_per_pixel < curve.evidence().points[i - 1].bits_per_pixel);
        }
    }

    void test_ssim_finds_the_same_ghost()
    {
        RecompressionOptions options = mjpeg_sweep();
        options.metric = Metric::Ssim;
        const RecompressionCurve curve = recompression_curve(compressed_once(6), options);
        assert(curve.evidence().estimated_prior_parameter == 6.0);
        assert(curve.configuration.metric == Metric::Ssim);
    }

    void test_the_sweep_is_sorted_and_recorded()
    {
        RecompressionOptions options;
        options.codec = ImageCodec::Mjpeg;
        options.parameter_range = {8, 3, 5, 3};
        const RecompressionCurve curve = recompression_curve(compressed_once(5), options);
        assert(curve.evidence().points.size() == 3);
        assert(curve.evidence().points[0].quality_parameter == 3 && curve.evidence().points[2].quality_parameter == 8);

        const StageRecord& record = curve.record;
        assert(record.kind() == StageKind::RecompressionCurve);
        assert(record.implementation == "mjpeg+mjpeg");
        assert(curve.evidence().notch_depths.size() == 3);
        assert(curve.evidence().quality_scale.starts_with("qscale"));
        assert(curve.to_json().contains("record"));

        // The configuration is what the sweep ran with: sorted without
        // repeats, and the format and color it picked filled in.
        assert(curve.configuration.parameter_range == (std::vector<double>{3, 5, 8}));
        assert(curve.configuration.pixel_format.has_value() && curve.configuration.color.has_value());

        // Duplicates collapse, which can leave too few points.
        options.parameter_range = {3, 3, 4};
        try
        {
            static_cast<void>(recompression_curve(compressed_once(5), options));
            assert(false && "expected ConfigError");
        }
        catch (const ConfigError&)
        {
        }
    }

    void test_a_frame_is_converted_once_to_the_requested_format()
    {
        const Frame rgb = decode_image(Source::from_path(data_path("testsrc_64x48.png"))).frame;
        RecompressionOptions options = mjpeg_sweep();
        options.pixel_format = PixelFormat::from_name("yuvj420p");
        options.color = bt601_yuv(ColorRange::Full);
        const RecompressionCurve curve = recompression_curve(rgb, options);
        assert(!curve.record.conversions.empty());
        assert(curve.record.input.pixel_format.name() == "rgb24");
        assert(curve.record.output.pixel_format.name() == "yuvj420p");
    }

    void test_a_webp_ghost()
    {
        if (!capabilities().supports(ImageCodec::WebP))
        {
            return;
        }
        const Frame source =
            convert(pristine(), PixelFormat::from_name("yuv420p"), bt601_yuv(ColorRange::Limited)).frame;
        EncodeImageOptions encode;
        encode.codec = ImageCodec::WebP;
        encode.pixel_format = PixelFormat::from_name("yuv420p");
        encode.rate_control = RateControl::quality(60);
        const Frame once = roundtrip(source, encode).frame;

        RecompressionOptions options;
        options.codec = ImageCodec::WebP;
        for (int quality = 30; quality <= 90; quality += 5)
        {
            options.parameter_range.push_back(quality);
        }
        const RecompressionCurve ghost = recompression_curve(once, options);
        assert(ghost.evidence().estimated_prior_parameter == 60.0);
        assert(!recompression_curve(source, options).evidence().estimated_prior_parameter.has_value());
    }

    void test_an_avif_ghost_under_the_same_encoder_and_settings()
    {
        // Checked with libaom only.
        if (!capabilities().supports(ImageCodec::Avif) ||
            capabilities().require_encoder(ImageCodec::Avif).name != "libaom-av1")
        {
            return;
        }
        const std::map<std::string, std::string> encoder_options = {{"cpu-used", "6"}};
        const Frame source =
            convert(pristine(), PixelFormat::from_name("yuv420p"), bt601_yuv(ColorRange::Limited)).frame;
        RecompressionOptions options;
        options.codec = ImageCodec::Avif;
        options.encoder_options = encoder_options;
        for (int crf = 3; crf <= 63; crf += 4)
        {
            options.parameter_range.push_back(crf);
        }
        for (const int prior : {11, 19, 31, 43})
        {
            EncodeImageOptions encode;
            encode.codec = ImageCodec::Avif;
            encode.pixel_format = PixelFormat::from_name("yuv420p");
            encode.rate_control = RateControl::quality(prior);
            encode.encoder_options = encoder_options;
            const RecompressionCurve ghost = recompression_curve(roundtrip(source, encode).frame, options);
            assert(ghost.evidence().estimated_prior_parameter == static_cast<double>(prior));
        }

        // A never-compressed frame can show a notch too (here at crf 59, with
        // confidence 0.55), so an AVIF notch alone does not prove a prior.
    }

    void test_a_jpeg_xl_ghost_under_the_same_encoder_and_settings()
    {
        if (!capabilities().supports(ImageCodec::Jxl))
        {
            return;
        }
        const Frame source = decode_image(Source::from_path(data_path("testsrc_64x48.png"))).frame;
        RecompressionOptions options;
        options.codec = ImageCodec::Jxl;
        for (int step = 1; step <= 12; ++step)
        {
            options.parameter_range.push_back(step * 0.5);
        }
        for (const double prior : {1.0, 2.0, 4.5})
        {
            EncodeImageOptions encode;
            encode.codec = ImageCodec::Jxl;
            encode.pixel_format = PixelFormat::from_name("rgb24");
            encode.rate_control = RateControl::quality(prior);
            const RecompressionCurve ghost = recompression_curve(roundtrip(source, encode).frame, options);
            assert(ghost.evidence().estimated_prior_parameter == prior);
        }
        assert(!recompression_curve(source, options).evidence().estimated_prior_parameter.has_value());
    }
}

int main()
{
    test_a_jpeg_ghost_reveals_the_prior_qscale();
    test_a_never_compressed_frame_shows_no_ghost();
    test_ssim_finds_the_same_ghost();
    test_the_sweep_is_sorted_and_recorded();
    test_a_frame_is_converted_once_to_the_requested_format();
    test_a_webp_ghost();
    test_an_avif_ghost_under_the_same_encoder_and_settings();
    test_a_jpeg_xl_ghost_under_the_same_encoder_and_settings();
    return 0;
}
