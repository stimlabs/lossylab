#include "lossylab/core/error.hpp"
#include "lossylab/measure/measure.hpp"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <string>
#include <vector>

using namespace lossylab;

namespace
{
    /// An 8-bit planar frame whose first plane comes from `first_plane` and
    /// whose other planes hold `other_planes`.
    Frame planar_frame(const char* pixel_format, const std::function<int(int, int)>& first_plane,
                       const int other_planes = 128, const int width = 64, const int height = 48,
                       const ColorSpec& color = ColorSpec::bt709_limited())
    {
        Frame frame = Frame::allocate(width, height, PixelFormat::from_name(pixel_format), color);
        for (int plane_index = 0; plane_index < frame.plane_count(); ++plane_index)
        {
            const PlaneView plane = frame.plane(plane_index);
            for (int y = 0; y < plane.height; ++y)
            {
                for (int x = 0; x < plane.width * plane.components_per_pixel; ++x)
                {
                    plane.row(y)[x] =
                        static_cast<std::uint8_t>(plane_index == 0 ? first_plane(x, y) : other_planes);
                }
            }
        }
        return frame;
    }

    Frame flat_frame(const int luma, const char* pixel_format = "yuv444p")
    {
        return planar_frame(pixel_format, [luma](int, int) { return luma; });
    }

    int checkerboard(const int x, const int y)
    {
        return ((x / 4) + (y / 4)) % 2 == 0 ? 60 : 190;
    }

    double value_of(const CompareResult& result, const char* name, const std::size_t frame_index = 0)
    {
        const auto& values = result.evidence().frames.at(frame_index);
        const auto it = values.find(name);
        assert(it != values.end() && "metric value missing");
        return it->second;
    }

    bool near(const double left, const double right, const double tolerance = 1e-4)
    {
        return std::abs(left - right) <= tolerance;
    }

    void test_identical_frames_have_infinite_psnr_and_unit_ssim()
    {
        const Frame frame = planar_frame("yuv420p", checkerboard);
        const CompareResult result = compare(frame, frame, {Metric::Psnr, Metric::Ssim});
        assert(std::isinf(value_of(result, "psnr")));
        assert(value_of(result, "mse") == 0.0);
        assert(value_of(result, "ssim") == 1.0);
        assert(value_of(result, "ssim_y") == 1.0);
        assert(result.record.kind() == StageKind::Compare);
        assert(result.record.conversions.empty());
    }

    void test_psnr_reports_the_mean_squared_error_of_each_plane()
    {
        // Luma off by 4 everywhere, chroma untouched: MSE 16 on luma alone.
        const CompareResult result = compare(flat_frame(100), flat_frame(104), {Metric::Psnr});
        assert(near(value_of(result, "mse_y"), 16.0));
        assert(value_of(result, "mse_u") == 0.0 && value_of(result, "mse_v") == 0.0);
        assert(near(value_of(result, "psnr_y"), 10.0 * std::log10(255.0 * 255.0 / 16.0)));

        // The three 4:4:4 planes weigh the same.
        assert(near(value_of(result, "mse"), 16.0 / 3.0));
        assert(near(value_of(result, "psnr"), 10.0 * std::log10(255.0 * 255.0 / (16.0 / 3.0))));
    }

    void test_ssim_falls_with_the_distortion()
    {
        const Frame reference = planar_frame("yuv420p", checkerboard);
        const Frame slightly = planar_frame("yuv420p", [](int x, int y) { return checkerboard(x, y) + (x % 2) * 4; });
        const Frame strongly = planar_frame("yuv420p", [](int x, int y) { return checkerboard(x, y) + (x % 2) * 40; });

        const double slight_ssim = value_of(compare(reference, slightly, {Metric::Ssim}), "ssim");
        const double strong_ssim = value_of(compare(reference, strongly, {Metric::Ssim}), "ssim");
        assert(slight_ssim < 1.0);
        assert(strong_ssim < slight_ssim);
    }

    void test_values_are_reported_per_frame_and_pooled()
    {
        const CompareResult result =
            compare({flat_frame(100), flat_frame(100)}, {flat_frame(102), flat_frame(104)}, {Metric::Psnr});
        assert(result.evidence().frames.size() == 2);
        assert(near(value_of(result, "mse_y", 0), 4.0));
        assert(near(value_of(result, "mse_y", 1), 16.0));

        const double first = value_of(result, "psnr", 0);
        const double second = value_of(result, "psnr", 1);
        const std::map<std::string, double>& pooled = result.evidence().pooled;
        assert(near(pooled.at("psnr_mean"), (first + second) / 2.0));
        assert(pooled.at("psnr_min") == second);
        assert(pooled.at("psnr_max") == first);
        assert(near(pooled.at("psnr_median"), (first + second) / 2.0));
        assert(near(pooled.at("psnr_std"), std::abs(first - second) / std::sqrt(2.0)));
    }

    void test_packed_rgb_is_refused_unless_conversion_is_allowed()
    {
        const Frame reference = planar_frame("rgb24", [](int x, int) { return x; }, 128, 64, 48, ColorSpec::srgb());
        const Frame distorted =
            planar_frame("rgb24", [](int x, int) { return x + 1; }, 128, 64, 48, ColorSpec::srgb());
        try
        {
            static_cast<void>(compare(reference, distorted, {Metric::Psnr}));
            assert(false && "expected ConversionRefused");
        }
        catch (const ConversionRefused& e)
        {
            assert(std::string(e.what()).find("psnr") != std::string::npos);
        }

        CompareOptions options;
        options.metrics = {Metric::Psnr, Metric::Ssim};
        options.strict = Strict::AllowRecorded;
        const CompareResult result = compare(reference, distorted, options);
        assert(result.evidence().measured_as.at("psnr") == "gbrp");
        assert(!result.record.conversions.empty());

        // Repacking loses nothing: every sample is off by exactly one.
        assert(near(value_of(result, "mse"), 1.0));
        assert(near(value_of(result, "mse_r"), 1.0) && near(value_of(result, "mse_g"), 1.0));
    }

    void test_frames_must_share_format_and_color()
    {
        const Frame reference = flat_frame(100);
        const Frame other_format = flat_frame(100, "yuv420p");
        Frame other_color = flat_frame(100);
        other_color.set_color(ColorSpec::bt601_limited());

        for (const Frame& distorted : {other_format, other_color})
        {
            try
            {
                static_cast<void>(compare(reference, distorted, {Metric::Psnr}));
                assert(false && "expected ConfigError");
            }
            catch (const ConfigError&)
            {
            }
        }
    }

    void test_ssim_needs_frames_of_at_least_eight_pixels()
    {
        const Frame tiny = planar_frame("yuv444p", checkerboard, 128, 4, 4);
        try
        {
            static_cast<void>(compare(tiny, tiny, {Metric::Ssim}));
            assert(false && "expected ConfigError");
        }
        catch (const ConfigError&)
        {
        }
        assert(std::isinf(value_of(compare(tiny, tiny, {Metric::Psnr}), "psnr")));
    }

    void test_the_json_carries_the_record()
    {
        const json::Value document = compare(flat_frame(100), flat_frame(104), {Metric::Psnr}).to_json();
        assert(document.contains("record"));
        assert(document.at("configuration").at("metrics").at(0).get<std::string>() == "psnr");
        assert(document.at("record").at("evidence").at("pooled").contains("psnr_mean"));
    }

    void test_no_metric_is_refused()
    {
        try
        {
            static_cast<void>(compare(flat_frame(100), flat_frame(104), CompareOptions{}));
            assert(false && "expected ConfigError");
        }
        catch (const ConfigError&)
        {
        }
    }
}

int main()
{
    test_no_metric_is_refused();
    test_identical_frames_have_infinite_psnr_and_unit_ssim();
    test_psnr_reports_the_mean_squared_error_of_each_plane();
    test_ssim_falls_with_the_distortion();
    test_values_are_reported_per_frame_and_pooled();
    test_packed_rgb_is_refused_unless_conversion_is_allowed();
    test_frames_must_share_format_and_color();
    test_ssim_needs_frames_of_at_least_eight_pixels();
    test_the_json_carries_the_record();
    return 0;
}
