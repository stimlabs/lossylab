#include "lossylab/codec/encode.hpp"
#include "lossylab/convert/convert.hpp"
#include "lossylab/core/error.hpp"
#include "lossylab/env/capabilities.hpp"
#include "lossylab/io/decode_image.hpp"
#include "lossylab/measure/compression_history.hpp"
#include "lossylab/measure/measure.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <random>
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

    /// Random values on a coarse grid, interpolated bilinearly.
    class ValueNoise
    {
    public:
        ValueNoise(const int cell, const int width, const int height, std::mt19937& engine)
            : m_cell(cell), m_columns(width / cell + 2), m_values(static_cast<std::size_t>(m_columns) *
                                                                  static_cast<std::size_t>(height / cell + 2))
        {
            for (double& value : m_values)
            {
                value = static_cast<double>(engine()) / static_cast<double>(std::mt19937::max()) - 0.5;
            }
        }

        [[nodiscard]] double at(const int x, const int y) const
        {
            const double grid_x = static_cast<double>(x) / m_cell;
            const double grid_y = static_cast<double>(y) / m_cell;
            const int left = static_cast<int>(grid_x);
            const int top = static_cast<int>(grid_y);
            const double fraction_x = grid_x - left;
            const double fraction_y = grid_y - top;
            const auto value = [this](const int column, const int row)
            { return m_values[static_cast<std::size_t>(row * m_columns + column)]; };
            const double upper = value(left, top) * (1 - fraction_x) + value(left + 1, top) * fraction_x;
            const double lower = value(left, top + 1) * (1 - fraction_x) + value(left + 1, top + 1) * fraction_x;
            return upper * (1 - fraction_y) + lower * fraction_y;
        }

    private:
        int m_cell;
        int m_columns;
        std::vector<double> m_values;
    };

    /// A never-compressed rgb24 image with the roughly 1/f spectrum of a
    /// photo: value noise at halving cell sizes and amplitudes, with
    /// independent color detail, plus a little sensor-like noise.
    Frame texture(const int width = 384, const int height = 256, const unsigned seed = 7)
    {
        std::mt19937 engine(seed);
        std::vector<std::array<ValueNoise, 3>> octaves;
        for (int cell = 64; cell >= 2; cell /= 2)
        {
            octaves.push_back({ValueNoise(cell, width, height, engine), ValueNoise(cell, width, height, engine),
                               ValueNoise(cell, width, height, engine)});
        }

        Frame frame = Frame::allocate(width, height, PixelFormat::from_name("rgb24"), ColorSpec::srgb());
        const PlaneView plane = frame.plane(0);
        for (int y = 0; y < height; ++y)
        {
            std::uint8_t* row = plane.row(y);
            for (int x = 0; x < width; ++x)
            {
                double luma = 128.0;
                double red_offset = 0.0;
                double blue_offset = 0.0;
                double amplitude = 140.0;
                for (const auto& octave : octaves)
                {
                    luma += amplitude * octave[0].at(x, y);
                    red_offset += 0.6 * amplitude * octave[1].at(x, y);
                    blue_offset += 0.6 * amplitude * octave[2].at(x, y);
                    amplitude /= 2.0;
                }
                const double noise = 3.0 * (static_cast<double>(engine()) / std::mt19937::max() - 0.5);
                const std::array<double, 3> rgb = {luma + red_offset + noise, luma - 0.5 * (red_offset + blue_offset),
                                                   luma + blue_offset - noise};
                for (std::size_t channel = 0; channel < 3; ++channel)
                {
                    row[3 * x + static_cast<int>(channel)] =
                        static_cast<std::uint8_t>(std::clamp(std::lround(rgb[channel]), 1L, 254L));
                }
            }
        }
        return frame;
    }

    ColorSpec jpeg_color()
    {
        ColorSpec color = ColorSpec::jpeg();
        color.primaries = ColorPrimaries::Bt709;
        color.transfer = TransferCharacteristic::Srgb;
        return color;
    }

    /// An rgb24 frame after one MJPEG generation at `qscale` in
    /// `pixel_format`, as a lossless save of the decoded JPEG holds it.
    Frame after_mjpeg(const Frame& rgb, const int qscale, const char* pixel_format)
    {
        EncodeImageOptions options;
        options.codec = ImageCodec::Mjpeg;
        options.pixel_format = PixelFormat::from_name(pixel_format);
        options.color = jpeg_color();
        options.rate_control = RateControl::quality(qscale);
        options.strict = Strict::AllowRecorded;
        const Frame decoded = roundtrip(rgb, options).frame;
        return convert(decoded, PixelFormat::from_name("rgb24"), rgb.color()).frame;
    }

    Frame crop_rgb(const Frame& rgb, const int left, const int top)
    {
        Frame cropped = Frame::allocate(rgb.width() - left, rgb.height() - top, rgb.pixel_format(), rgb.color());
        for (int y = 0; y < cropped.height(); ++y)
        {
            std::memcpy(cropped.plane(0).row(y), rgb.plane(0).row(y + top) + 3 * left,
                        static_cast<std::size_t>(3 * cropped.width()));
        }
        return cropped;
    }

    CompressionHistoryOptions without_recompression()
    {
        CompressionHistoryOptions options;
        options.recompression_codecs.clear();
        return options;
    }

    bool has_trace(const CompressionHistory& history, const TraceEvidence evidence)
    {
        return std::any_of(history.traces.begin(), history.traces.end(),
                           [evidence](const CompressionTrace& trace) { return trace.evidence == evidence; });
    }

    void test_a_never_compressed_image_shows_no_trace()
    {
        const CompressionHistory history = compression_history(texture());
        assert(history.traces.empty());
        assert(history.jpeg.has_value() && !history.jpeg->detected);
        assert(history.chroma.has_value() && history.chroma->subsampling == Subsampling::Yuv444);
        assert(history.record.kind == StageKind::CompressionHistory);
        assert(history.record.params.at("analyzed_as").get<std::string>() == "rgb24");
    }

    void test_a_jpeg_saved_as_rgb_shows_its_tables_and_subsampling()
    {
        const Frame pristine = texture();
        for (const auto& [pixel_format, subsampling] :
             {std::pair{"yuvj420p", Subsampling::Yuv420}, std::pair{"yuvj422p", Subsampling::Yuv422},
              std::pair{"yuvj444p", Subsampling::Yuv444}})
        {
            const CompressionHistory history =
                compression_history(after_mjpeg(pristine, 4, pixel_format), without_recompression());
            assert(history.jpeg->detected);
            assert(history.jpeg->grid_x == 0 && history.jpeg->grid_y == 0);
            assert(history.jpeg->chroma_subsampling == subsampling);

            // FFmpeg's MJPEG scales MPEG-1's intra matrix by qscale / 8: 16,
            // 19 and 22 become 8, 9 and 11 at qscale 4.
            assert(history.jpeg->luma.determined >= 5);
            assert(history.jpeg->luma.values[1] == 8 && history.jpeg->luma.values[8] == 8);
            assert(history.jpeg->luma.values[2] == 9 && history.jpeg->luma.values[16] == 9);

            // FFmpeg's MJPEG uses MPEG-1's intra matrix, not libjpeg's
            // tables, so the libjpeg quality is only the nearest one.
            assert(history.jpeg->ijg_match < 1.0);

            const auto jpeg = std::find_if(history.traces.begin(), history.traces.end(),
                                           [](const CompressionTrace& trace)
                                           { return trace.evidence == TraceEvidence::JpegQuantization; });
            assert(jpeg != history.traces.end());
            assert(jpeg->codec == ImageCodec::Mjpeg);
            assert(jpeg->subsampling == subsampling);
        }
    }

    void test_chroma_upsampling_is_found_at_any_quality()
    {
        // swscale replicates chroma going to RGB, whatever the JPEG's quality.
        for (const int qscale : {2, 8})
        {
            const CompressionHistory history =
                compression_history(after_mjpeg(texture(), qscale, "yuvj420p"), without_recompression());
            assert(history.chroma->subsampling == Subsampling::Yuv420);
            assert(history.chroma->upsampling == ChromaUpsampling::Replicate);
            assert(has_trace(history, TraceEvidence::ChromaSubsampling));
        }
    }

    void test_a_crop_after_compression_moves_the_grid()
    {
        const Frame cropped = crop_rgb(after_mjpeg(texture(), 4, "yuvj420p"), 5, 3);
        const CompressionHistory history = compression_history(cropped, without_recompression());
        assert(history.jpeg->detected);
        assert(history.jpeg->grid_x == 3 && history.jpeg->grid_y == 5);
        assert(history.jpeg->chroma_subsampling == Subsampling::Yuv420);
    }

    void test_a_jpeg_still_in_its_yuv_is_read_directly()
    {
        EncodeImageOptions options;
        options.codec = ImageCodec::Mjpeg;
        options.pixel_format = PixelFormat::from_name("yuvj420p");
        options.color = jpeg_color();
        options.rate_control = RateControl::quality(4);
        options.strict = Strict::AllowRecorded;
        const Frame yuv = roundtrip(texture(), options).frame;

        const CompressionHistory history = compression_history(yuv, without_recompression());
        assert(history.record.params.at("analyzed_as").get<std::string>() == "yuvj420p");
        assert(history.record.conversions.empty());
        assert(!history.chroma.has_value());
        assert(history.jpeg->detected);
        assert(history.jpeg->chroma_subsampling == Subsampling::Yuv420);
        assert(history.jpeg->chroma.has_value());
    }

    void test_gray_and_alpha_frames_are_analyzed()
    {
        const Frame jpeg = after_mjpeg(texture(), 4, "yuvj444p");

        const Frame gray = convert(jpeg, PixelFormat::from_name("gray"), jpeg_color()).frame;
        const CompressionHistory gray_history = compression_history(gray, without_recompression());
        assert(gray_history.record.params.at("analyzed_as").get<std::string>() == "gray");
        assert(gray_history.jpeg->detected);
        assert(gray_history.jpeg->chroma_subsampling == Subsampling::Gray);
        assert(!gray_history.chroma.has_value());

        const Frame rgba = convert(jpeg, PixelFormat::from_name("rgba"), jpeg.color()).frame;
        const CompressionHistory rgba_history = compression_history(rgba, without_recompression());
        assert(!rgba_history.record.conversions.empty());
        assert(rgba_history.jpeg->detected);
    }

    void test_an_achromatic_frame_claims_no_chroma_layout()
    {
        const Frame jpeg = after_mjpeg(texture(), 4, "yuvj444p");
        const Frame gray = convert(jpeg, PixelFormat::from_name("gray"), jpeg_color()).frame;
        const Frame rgb = convert(gray, PixelFormat::from_name("rgb24"), jpeg.color()).frame;
        const CompressionHistory history = compression_history(rgb, without_recompression());
        assert(history.record.params.at("chroma").get<std::string>() == "achromatic");
        assert(history.jpeg->detected);
        assert(!history.jpeg->chroma_subsampling.has_value());
        assert(!history.chroma.has_value());
    }

    void test_a_webp_saved_as_rgb_shows_its_quality()
    {
        if (!capabilities().supports(ImageCodec::WebP))
        {
            return;
        }
        const Frame pristine = texture();
        ColorSpec webp_color = jpeg_color();
        webp_color.range = ColorRange::Limited;
        EncodeImageOptions options;
        options.codec = ImageCodec::WebP;
        options.pixel_format = PixelFormat::from_name("yuv420p");
        options.color = webp_color;
        options.rate_control = RateControl::quality(80);
        options.strict = Strict::AllowRecorded;
        const Frame decoded = roundtrip(pristine, options).frame;
        const Frame rgb = convert(decoded, PixelFormat::from_name("rgb24"), pristine.color()).frame;

        const CompressionHistory history = compression_history(rgb);
        assert(!history.jpeg->detected);
        assert(history.chroma->subsampling == Subsampling::Yuv420);
        const auto webp = std::find_if(history.traces.begin(), history.traces.end(),
                                       [](const CompressionTrace& trace)
                                       { return trace.evidence == TraceEvidence::Recompression; });
        assert(webp != history.traces.end());
        assert(webp->codec == ImageCodec::WebP);
        assert(webp->quality.has_value() && std::abs(*webp->quality - 80.0) <= 3.0);
        assert(history.recompression_curves.size() == 2);
    }

    void test_the_record_serializes()
    {
        const CompressionHistory history = compression_history(after_mjpeg(texture(), 4, "yuvj420p"));
        const json::Value document = history.to_json();
        assert(document.at("jpeg").at("detected").get<bool>());
        const json::Value& traces = document.at("traces");
        assert(std::any_of(traces.begin(), traces.end(), [](const json::Value& trace)
                           { return trace.at("evidence").get<std::string>() == "jpeg_quantization"; }));
        assert(document.at("record").at("kind").get<std::string>() == "compression_history");
        assert(document.at("record").at("params").at("jpeg_quantization").at("luma_grid_scores").size() == 64);
    }

    void test_bad_options_are_refused()
    {
        CompressionHistoryOptions options;
        options.recompression_crop = -1;
        try
        {
            static_cast<void>(compression_history(texture(64, 64), options));
            assert(false && "expected ConfigError");
        }
        catch (const ConfigError&)
        {
        }

        // A codec without a quality scale is reported, not thrown.
        options.recompression_crop = 0;
        options.recompression_codecs = {ImageCodec::Png};
        const CompressionHistory history = compression_history(texture(64, 64), options);
        assert(history.record.params.at("recompression").at("errors").contains("png"));
    }

    // -----------------------------------------------------------------------
    // recompression_curve() on any input
    // -----------------------------------------------------------------------

    RecompressionOptions mjpeg_sweep()
    {
        RecompressionOptions options;
        options.codec = ImageCodec::Mjpeg;
        options.parameter_range = {2, 4, 6, 8};
        return options;
    }

    void test_recompression_takes_rgb_gray_and_alpha()
    {
        const Frame rgb = texture(64, 48);
        const RecompressionCurve from_rgb = recompression_curve(rgb, mjpeg_sweep());
        assert(from_rgb.record.output.pixel_format.name() == "yuvj420p");
        assert(from_rgb.record.params.at("alpha").get<std::string>() == "kept");

        const Frame rgba = convert(rgb, PixelFormat::from_name("rgba"), rgb.color()).frame;
        const RecompressionCurve from_rgba = recompression_curve(rgba, mjpeg_sweep());
        assert(from_rgba.record.params.at("alpha").get<std::string>() == "dropped");

        const Frame gray = convert(rgb, PixelFormat::from_name("gray"), jpeg_color()).frame;
        RecompressionOptions luma_only = mjpeg_sweep();
        luma_only.planes = RecompressionPlanes::Luma;
        const RecompressionCurve from_gray = recompression_curve(gray, luma_only);
        assert(from_gray.record.output.pixel_format.name() == "yuvj444p");
        assert(from_gray.record.params.at("error").get<std::string>() == "mse_y");

        if (capabilities().supports(ImageCodec::WebP))
        {
            RecompressionOptions webp;
            webp.codec = ImageCodec::WebP;
            webp.parameter_range = {50, 70, 90};
            const RecompressionCurve curve = recompression_curve(rgba, webp);
            assert(curve.record.output.pixel_format.name() == "yuv420p");
            assert(curve.record.output.color.range == ColorRange::Limited);
        }
    }

    void test_luma_error_needs_a_yuv_format()
    {
        if (!capabilities().supports(ImageCodec::Jxl))
        {
            return;
        }
        RecompressionOptions options;
        options.codec = ImageCodec::Jxl;
        options.parameter_range = {1, 2, 3};
        options.planes = RecompressionPlanes::Luma;
        try
        {
            static_cast<void>(recompression_curve(texture(64, 48), options));
            assert(false && "expected ConfigError");
        }
        catch (const ConfigError&)
        {
        }
    }

    void test_unspecified_color_is_filled_from_the_codec()
    {
        if (!capabilities().supports(ImageCodec::WebP))
        {
            return;
        }
        const FrameResult decoded = decode_image(Source::from_path(data_path("testsrc_64x48_lossy.webp")));
        assert(decoded.frame.color().chroma_location == ChromaLocation::Center);
        assert(std::any_of(decoded.record.conversions.begin(), decoded.record.conversions.end(),
                           [](const ConversionEvent& event) { return event.performed_by == "codec_implied_color"; }));

        Frame untagged = decoded.frame;
        ColorSpec color = untagged.color();
        color.chroma_location = ChromaLocation::Unspecified;
        untagged.set_color(color);
        untagged.sync_color_to_av_frame();

        RecompressionOptions options;
        options.codec = ImageCodec::WebP;
        options.parameter_range = {50, 70, 90};
        const RecompressionCurve curve = recompression_curve(untagged, options);
        assert(curve.record.conversions.front().performed_by == "codec_implied_color");
        assert(curve.record.output.color.chroma_location == ChromaLocation::Center);
    }

    void test_a_lossless_codec_cannot_be_swept()
    {
        RecompressionOptions options;
        options.codec = ImageCodec::Png;
        options.parameter_range = {1, 2, 3};
        try
        {
            static_cast<void>(recompression_curve(texture(64, 48), options));
            assert(false && "expected ConfigError");
        }
        catch (const ConfigError&)
        {
        }
    }
}

int main()
{
    test_a_never_compressed_image_shows_no_trace();
    test_a_jpeg_saved_as_rgb_shows_its_tables_and_subsampling();
    test_chroma_upsampling_is_found_at_any_quality();
    test_a_crop_after_compression_moves_the_grid();
    test_a_jpeg_still_in_its_yuv_is_read_directly();
    test_gray_and_alpha_frames_are_analyzed();
    test_an_achromatic_frame_claims_no_chroma_layout();
    test_a_webp_saved_as_rgb_shows_its_quality();
    test_the_record_serializes();
    test_bad_options_are_refused();
    test_recompression_takes_rgb_gray_and_alpha();
    test_luma_error_needs_a_yuv_format();
    test_unspecified_color_is_filled_from_the_codec();
    test_a_lossless_codec_cannot_be_swept();
    return 0;
}
