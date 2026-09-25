#include "lossylab/core/error.hpp"
#include "lossylab/env/log.hpp"
#include "lossylab/io/decode_image.hpp"
#include "lossylab/measure/measure.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <random>
#include <string>
#include <vector>

using namespace lossylab;

namespace
{
    std::string data_path(std::string_view name)
    {
        return std::string(LOSSYLAB_TEST_DATA_DIR) + "/" + std::string(name);
    }

    void write_sample(const PlaneView& plane, const int x, const int y, const int value)
    {
        std::uint8_t* row = plane.row(y);
        if (plane.bytes_per_sample == 1)
        {
            row[x] = static_cast<std::uint8_t>(value);
            return;
        }
        const auto sample = static_cast<std::uint16_t>(value);
        std::memcpy(row + static_cast<std::size_t>(x) * 2, &sample, 2);
    }

    /// A planar YUV or gray frame whose luma comes from `luma`, with neutral
    /// chroma.
    Frame planar_frame(const int width, const int height, const char* pixel_format,
                       const std::function<int(int, int)>& luma, const ColorSpec& color = ColorSpec::bt709_limited())
    {
        Frame frame = Frame::allocate(width, height, PixelFormat::from_name(pixel_format), color);
        const int neutral_chroma = 1 << (frame.pixel_format().bit_depth() - 1);
        for (int plane_index = 0; plane_index < frame.plane_count(); ++plane_index)
        {
            const PlaneView plane = frame.plane(plane_index);
            for (int y = 0; y < plane.height; ++y)
            {
                for (int x = 0; x < plane.width; ++x)
                {
                    write_sample(plane, x, y, plane_index == 0 ? luma(x, y) : neutral_chroma);
                }
            }
        }
        return frame;
    }

    Frame flat_frame(const int luma, const char* pixel_format = "yuv420p")
    {
        return planar_frame(64, 48, pixel_format, [luma](int, int) { return luma; });
    }

    /// Luma 50 and 200 in squares of `size` pixels.
    int checkerboard(const int x, const int y, const int size)
    {
        return ((x / size) + (y / size)) % 2 == 0 ? 50 : 200;
    }

    int clamp_to_8_bits(const double value)
    {
        return std::clamp(static_cast<int>(std::lround(value)), 0, 255);
    }

    /// A measurement the analyzer must have produced.
    template <typename Measurement>
    const Measurement& present(const std::optional<Measurement>& measurement)
    {
        assert(measurement.has_value() && "measurement missing");
        return *measurement;
    }

    const SignalLevels& signal_levels_of(const MeasureResult& result, const std::size_t frame_index = 0)
    {
        return present(result.frames.at(frame_index).signal_levels);
    }

    void test_signal_levels_report_the_levels_of_the_planes()
    {
        Frame frame = flat_frame(100, "yuv444p");
        write_sample(frame.plane(0), 3, 4, 20);
        write_sample(frame.plane(0), 10, 11, 230);

        const MeasureResult result = measure(frame, {Analyzer::SignalLevels});
        const SignalLevels& levels = signal_levels_of(result);
        assert(levels.luma.minimum == 20.0);
        assert(levels.luma.maximum == 230.0);
        assert(levels.luma.percentile_10 == 100.0);
        assert(levels.luma.percentile_90 == 100.0);
        assert(levels.chroma_u.minimum == 128.0 && levels.chroma_u.maximum == 128.0);
        assert(levels.chroma_v.mean == 128.0);
        assert(levels.saturation.maximum == 0.0);
        assert(levels.outside_limited_range == 0.0);
        // Bits left once the low bits that are zero in every sample are dropped.
        assert(levels.luma_bit_depth == 6);
        assert(levels.chroma_u_bit_depth == 1 && levels.chroma_v_bit_depth == 1);
        assert(!result.frames.front().blockiness.has_value());
        assert(!result.frames.front().letterbox.has_value());
        assert(result.record.conversions.empty());
        assert(result.record.params.at("measured_as").at("signal_levels") == "yuv444p");
    }

    void test_signal_levels_count_pixels_outside_the_limited_range()
    {
        Frame frame = flat_frame(100, "yuv444p");
        write_sample(frame.plane(0), 0, 0, 5);
        write_sample(frame.plane(0), 1, 0, 250);

        const MeasureResult result = measure(frame, {Analyzer::SignalLevels});
        assert(std::abs(signal_levels_of(result).outside_limited_range - 2.0 / (64.0 * 48.0)) < 1e-9);
    }

    void test_signal_levels_stay_in_the_frames_own_bit_depth()
    {
        const Frame frame = planar_frame(64, 48, "yuv420p10", [](int x, int) { return 64 + x; });
        const MeasureResult result = measure(frame, {Analyzer::SignalLevels});
        const SignalLevels& levels = signal_levels_of(result);
        assert(levels.luma.minimum == 64.0);
        assert(levels.luma.maximum == 127.0);
        assert(levels.chroma_u.mean == 512.0);
        assert(result.record.conversions.empty());
    }

    void test_letterbox_finds_the_content_inside_black_bars()
    {
        std::mt19937 generator(7);
        std::uniform_int_distribution<int> content_luma(60, 200);
        const Frame frame = planar_frame(64, 48, "yuv420p",
                                         [&](const int x, const int y)
                                         {
                                             const bool in_bar = y < 6 || y >= 42 || x < 4 || x >= 60;
                                             return in_bar ? 16 : content_luma(generator);
                                         });

        const MeasureResult result = measure(frame, {Analyzer::Letterbox});
        const Letterbox& letterbox = present(result.frames.front().letterbox);
        assert(letterbox.content_rect.x == 4.0 && letterbox.content_rect.y == 6.0);
        assert(letterbox.content_rect.width == 56.0 && letterbox.content_rect.height == 36.0);
        const LetterboxBars& bars = present(letterbox.bars);
        assert(bars.top == 6 && bars.bottom == 6 && bars.left == 4 && bars.right == 4);
        assert(std::abs(letterbox.content_fraction - (56.0 * 36.0) / (64.0 * 48.0)) < 1e-9);
    }

    void test_a_frame_without_bars_is_all_content()
    {
        const MeasureResult result = measure(flat_frame(128), {Analyzer::Letterbox});
        const Letterbox& letterbox = present(result.frames.front().letterbox);
        assert(letterbox.content_fraction == 1.0);
        const LetterboxBars& bars = present(letterbox.bars);
        assert(bars.top == 0 && bars.bottom == 0 && bars.left == 0 && bars.right == 0);
    }

    void test_a_black_frame_has_no_content()
    {
        const MeasureResult result = measure(flat_frame(16), {Analyzer::Letterbox});
        const Letterbox& letterbox = present(result.frames.front().letterbox);
        assert(letterbox.content_rect.width == 0.0 && letterbox.content_rect.height == 0.0);
        assert(letterbox.content_fraction == 0.0);
        assert(!letterbox.bars.has_value());
    }

    void test_blockiness_is_higher_for_an_eight_pixel_block_grid()
    {
        std::mt19937 generator(11);
        std::uniform_int_distribution<int> block_luma(40, 200);
        std::normal_distribution<double> noise(0.0, 2.0);

        std::vector<int> block_values(16 * 16);
        for (int& value : block_values)
        {
            value = block_luma(generator);
        }
        const Frame blocky = planar_frame(128, 128, "yuv420p",
                                          [&](const int x, const int y)
                                          {
                                              const int block = (y / 8) * 16 + (x / 8);
                                              return clamp_to_8_bits(block_values[static_cast<std::size_t>(block)] +
                                                                     noise(generator));
                                          });
        const Frame smooth = planar_frame(128, 128, "yuv420p", [&](const int x, const int y)
                                          { return clamp_to_8_bits(60.0 + x + y / 2.0 + noise(generator)); });

        const double blocky_value = present(measure(blocky, {Analyzer::Blockiness}).frames.front().blockiness);
        const double smooth_value = present(measure(smooth, {Analyzer::Blockiness}).frames.front().blockiness);
        assert(smooth_value < 1.5);
        assert(blocky_value > 3.0 * smooth_value);
    }

    void test_blurriness_grows_with_blur()
    {
        const auto sharp_luma = [](const int x, const int y) { return checkerboard(x, y, 32); };
        const Frame sharp = planar_frame(128, 128, "yuv420p", sharp_luma);

        // A 9x9 box blur of the same checkerboard.
        const Frame blurred = planar_frame(128, 128, "yuv420p",
                                           [&](const int x, const int y)
                                           {
                                               int total = 0;
                                               for (int dy = -4; dy <= 4; ++dy)
                                               {
                                                   for (int dx = -4; dx <= 4; ++dx)
                                                   {
                                                       total += sharp_luma(std::clamp(x + dx, 0, 127),
                                                                           std::clamp(y + dy, 0, 127));
                                                   }
                                               }
                                               return total / 81;
                                           });

        const double sharp_value = present(measure(sharp, {Analyzer::Blurriness}).frames.front().blurriness);
        const double blurred_value = present(measure(blurred, {Analyzer::Blurriness}).frames.front().blurriness);
        assert(blurred_value > 2.0 * sharp_value);
    }

    void test_a_frame_without_edges_has_no_blurriness()
    {
        const MeasureResult result = measure(flat_frame(128), {Analyzer::Blurriness});
        assert(!result.frames.front().blurriness.has_value());
    }

    Frame noisy_frame(const char* pixel_format, const double sigma, const int scale)
    {
        std::mt19937 generator(3);
        std::normal_distribution<double> noise(0.0, sigma * scale);
        return planar_frame(256, 256, pixel_format,
                            [&](int, int) { return static_cast<int>(std::lround(128.0 * scale + noise(generator))); });
    }

    void test_noise_estimates_the_sigma_of_added_gaussian_noise()
    {
        const double estimate =
            present(measure(noisy_frame("yuv420p", 5.0, 1), {Analyzer::Noise}).frames.front().noise_sigma);
        assert(std::abs(estimate - 5.0) < 0.5);
    }

    void test_noise_is_reported_in_8_bit_code_values_at_any_depth()
    {
        const double estimate =
            present(measure(noisy_frame("yuv420p10", 5.0, 4), {Analyzer::Noise}).frames.front().noise_sigma);
        assert(std::abs(estimate - 5.0) < 0.5);
    }

    void test_edges_alone_read_as_no_noise()
    {
        const Frame edges =
            planar_frame(128, 128, "yuv420p", [](const int x, const int y) { return checkerboard(x, y, 64); });
        assert(present(measure(edges, {Analyzer::Noise}).frames.front().noise_sigma) == 0.0);
        assert(present(measure(flat_frame(90), {Analyzer::Noise}).frames.front().noise_sigma) == 0.0);
    }

    Frame rgb_frame()
    {
        Frame frame = Frame::allocate(64, 48, PixelFormat::from_name("rgb24"),
                                      ColorSpec{ColorMatrix::Rgb, ColorRange::Full, ColorPrimaries::Bt709,
                                                TransferCharacteristic::Srgb, ChromaLocation::Unspecified});
        const PlaneView plane = frame.plane(0);
        for (int y = 0; y < plane.height; ++y)
        {
            for (int x = 0; x < plane.width * 3; ++x)
            {
                plane.row(y)[x] = static_cast<std::uint8_t>((x * 3 + y * 5) % 256);
            }
        }
        return frame;
    }

    void test_a_format_an_analyzer_cannot_take_is_refused_by_default()
    {
        try
        {
            (void)(measure(rgb_frame(), {Analyzer::SignalLevels}));
            assert(false && "expected throw");
        }
        catch (const ConversionRefused& e)
        {
            assert(e.from() == "pix_fmt rgb24");
            assert(e.to() == "pix_fmt yuv444p");
            assert(e.context().find("signalstats") != std::string::npos);
        }

        try
        {
            (void)(measure(flat_frame(100, "yuv420p10"), {Analyzer::Blockiness}));
            assert(false && "expected throw");
        }
        catch (const ConversionRefused& e)
        {
            assert(e.to() == "pix_fmt yuv420p");
        }
    }

    void test_allowing_conversion_measures_a_converted_copy_and_records_it()
    {
        MeasureOptions options;
        options.strict = Strict::AllowRecorded;
        const MeasureResult result =
            measure(rgb_frame(), {Analyzer::SignalLevels, Analyzer::Noise, Analyzer::Blockiness, Analyzer::Letterbox},
                    options);

        const json::Value& measured_as = result.record.params.at("measured_as");
        assert(measured_as.at("signal_levels") == "yuv444p");
        assert(measured_as.at("noise") == "yuv444p");
        assert(measured_as.at("blockiness") == "gbrp");
        assert(measured_as.at("letterbox") == "rgb24");
        assert(result.frames.front().signal_levels.has_value());
        assert(result.frames.front().noise_sigma.has_value());

        // One conversion to yuv444p, shared by signal levels and noise, and one
        // to gbrp.
        const auto pixel_format_changes =
            std::count_if(result.record.conversions.begin(), result.record.conversions.end(),
                          [](const ConversionEvent& event) { return event.property == "pix_fmt"; });
        assert(pixel_format_changes == 2);
        for (const ConversionEvent& event : result.record.conversions)
        {
            assert(event.cause == ConversionCause::CodecConstraint);
        }
    }

    void test_frames_must_share_one_format()
    {
        try
        {
            (void)(measure({flat_frame(100), flat_frame(100, "yuv444p")}, {Analyzer::SignalLevels}));
            assert(false && "expected throw");
        }
        catch (const ConfigError&)
        {
        }
    }

    void test_temporal_analyzers_are_not_implemented_yet()
    {
        for (const Analyzer analyzer : {Analyzer::Interlacing, Analyzer::SpatialTemporalInfo, Analyzer::SceneChange,
                                        Analyzer::DuplicateFrames})
        {
            try
            {
                (void)(measure(flat_frame(100), {Analyzer::SignalLevels, analyzer}));
                assert(false && "expected throw");
            }
            catch (const NotImplemented& e)
            {
                assert(e.symbol().find(to_string(analyzer)) != std::string::npos);
            }
        }
    }

    void test_each_frame_is_measured_and_the_values_pooled()
    {
        const MeasureResult result =
            measure({flat_frame(60), flat_frame(100), flat_frame(140)}, {Analyzer::SignalLevels});
        assert(result.frames.size() == 3);
        assert(result.frames[1].index == 1);
        assert(signal_levels_of(result, 0).luma.mean == 60.0);
        assert(signal_levels_of(result, 2).luma.mean == 140.0);

        const statistics::Summary& luma_mean = result.pooled.at("signal_levels.luma.mean");
        assert(luma_mean.count == 3);
        assert(luma_mean.mean == 100.0);
        assert(luma_mean.minimum == 60.0);
        assert(luma_mean.maximum == 140.0);
        assert(luma_mean.median == 100.0);
        assert(luma_mean.std == 40.0);
    }

    void test_pooled_std_needs_two_frames_and_the_median_of_an_even_count_is_the_midpoint()
    {
        const MeasureResult single = measure({flat_frame(60)}, {Analyzer::SignalLevels});
        assert(std::isnan(single.pooled.at("signal_levels.luma.mean").std));
        assert(single.pooled.at("signal_levels.luma.mean").median == 60.0);

        const MeasureResult pair = measure({flat_frame(60), flat_frame(100)}, {Analyzer::SignalLevels});
        assert(pair.pooled.at("signal_levels.luma.mean").median == 80.0);
    }

    void test_only_measurements_are_pooled_and_only_over_the_frames_that_have_them()
    {
        const MeasureResult result = measure({flat_frame(60), flat_frame(100)}, {Analyzer::Blurriness});
        assert(!result.pooled.contains("index"));
        assert(!result.pooled.contains("signal_levels.luma.mean"));

        // A flat frame has no edges, so no frame contributes a blurriness.
        assert(!result.pooled.contains("blurriness"));

        const MeasureResult letterboxed = measure({flat_frame(16), flat_frame(128)}, {Analyzer::Letterbox});
        assert(letterboxed.pooled.at("letterbox.content_fraction").count == 2);
        assert(letterboxed.pooled.at("letterbox.bars.top").count == 1);
    }

    void test_a_decoded_jpeg_measures_without_conversion()
    {
        const DecodedImage decoded = decode_image(Source::from_path(data_path("testsrc_64x48_q75.jpg")));
        const MeasureResult result =
            measure(decoded.frame, {Analyzer::SignalLevels, Analyzer::Blockiness, Analyzer::Blurriness,
                                    Analyzer::Noise, Analyzer::Letterbox});

        assert(result.record.kind == StageKind::Measure);
        assert(result.record.conversions.empty());
        assert(result.record.params.at("analyzers").size() == 5);
        const FrameMeasurement& measurement = result.frames.front();
        assert(measurement.signal_levels.has_value());
        assert(measurement.blockiness.has_value());
        assert(measurement.blurriness.has_value());
        assert(measurement.noise_sigma.has_value());
        assert(measurement.letterbox.has_value());
        assert(result.to_json().at("record").at("kind") == "measure");
    }

    void test_the_filters_per_frame_reports_stay_out_of_the_log()
    {
        const LogCapture log(LogLevel::Info);
        (void)(measure({flat_frame(100), flat_frame(120)},
                       {Analyzer::SignalLevels, Analyzer::Blockiness, Analyzer::Blurriness, Analyzer::Letterbox}));
        assert(log.messages().empty());
    }
}

int main()
{
    test_signal_levels_report_the_levels_of_the_planes();
    test_signal_levels_count_pixels_outside_the_limited_range();
    test_signal_levels_stay_in_the_frames_own_bit_depth();
    test_letterbox_finds_the_content_inside_black_bars();
    test_a_frame_without_bars_is_all_content();
    test_a_black_frame_has_no_content();
    test_blockiness_is_higher_for_an_eight_pixel_block_grid();
    test_blurriness_grows_with_blur();
    test_a_frame_without_edges_has_no_blurriness();
    test_noise_estimates_the_sigma_of_added_gaussian_noise();
    test_noise_is_reported_in_8_bit_code_values_at_any_depth();
    test_edges_alone_read_as_no_noise();
    test_a_format_an_analyzer_cannot_take_is_refused_by_default();
    test_allowing_conversion_measures_a_converted_copy_and_records_it();
    test_frames_must_share_one_format();
    test_temporal_analyzers_are_not_implemented_yet();
    test_each_frame_is_measured_and_the_values_pooled();
    test_pooled_std_needs_two_frames_and_the_median_of_an_even_count_is_the_midpoint();
    test_only_measurements_are_pooled_and_only_over_the_frames_that_have_them();
    test_a_decoded_jpeg_measures_without_conversion();
    test_the_filters_per_frame_reports_stay_out_of_the_log();
    return 0;
}
