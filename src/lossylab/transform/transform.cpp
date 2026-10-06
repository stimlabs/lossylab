#include "lossylab/transform/transform.hpp"

#include "lossylab/core/error.hpp"
#include "lossylab/detail/ff_error.hpp"
#include "lossylab/detail/ff_ptr.hpp"
#include "lossylab/io/orientation.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

extern "C" {
#include <libavutil/frame.h>
}

namespace lossylab
{
    FrameResult crop(const Frame& frame, const CropOptions& options, const std::optional<BlockGrid>& block_grid)
    {
        const detail::StageClock clock;
        if (frame.empty())
        {
            throw ConfigError("crop() received an empty frame");
        }
        if (options.width <= 0 || options.height <= 0 || options.x < 0 || options.y < 0 ||
            options.x + options.width > frame.width() || options.y + options.height > frame.height())
        {
            throw ConfigError("crop() of " + std::to_string(options.width) + "x" + std::to_string(options.height) +
                              " at (" + std::to_string(options.x) + ", " + std::to_string(options.y) +
                              ") does not fit in a " + std::to_string(frame.width()) + "x" +
                              std::to_string(frame.height()) + " frame");
        }

        Frame output = Frame::allocate(options.width, options.height, frame.pixel_format(), frame.color());
        detail::copy_rectangle(*frame.raw(), options.x, options.y, *output.raw(), 0, 0, options.width,
                               options.height);
        output.set_pts(frame.pts());
        output.set_time_base(frame.time_base());
        output.copy_embedded_from(frame);

        StageRecord record;
        record.implementation = "lossylab";
        record.input = frame.describe();
        record.transform = CoordinateTransform::crop(options.x, options.y);
        CropEvidence evidence;
        if (block_grid.has_value())
        {
            evidence.block_grid = block_grid->apply_transform(record.transform);
            evidence.whole_blocks =
                options.width % block_grid->block_width == 0 && options.height % block_grid->block_height == 0;
        }
        record.evidence = evidence;
        record.output = output.describe();
        record.duration_ms = clock.duration_ms();
        record.ffmpeg_duration_ms = clock.ffmpeg_duration_ms();
        return FrameResult{std::move(output), std::move(record), options};
    }

    FrameResult orient(const Frame& frame, const OrientOptions& options)
    {
        const detail::StageClock clock;
        if (frame.empty())
        {
            throw ConfigError("orient() received an empty frame");
        }
        if (options.orientation < 1 || options.orientation > 8)
        {
            throw ConfigError("orient() takes an EXIF orientation from 1 to 8, got " +
                              std::to_string(options.orientation));
        }

        StageRecord record;
        record.implementation = "lossylab";
        record.input = frame.describe();
        Frame output = options.orientation == 1
                           ? frame
                           : detail::apply_orientation(frame, options.orientation, options.strict, record.conversions);
        output.set_orientation(std::nullopt);
        record.transform = CoordinateTransform::orientation(options.orientation, frame.width(), frame.height());
        record.evidence = OrientEvidence{frame.orientation()};
        record.output = output.describe();
        record.duration_ms = clock.duration_ms();
        record.ffmpeg_duration_ms = clock.ffmpeg_duration_ms();
        return FrameResult{std::move(output), std::move(record), options};
    }

    FrameResult achromatic(const Frame& frame, const AchromaticOptions& options)
    {
        const detail::StageClock clock;
        if (frame.empty())
        {
            throw ConfigError("achromatic() received an empty frame");
        }
        if (frame.pixel_format() != PixelFormat::from_name("rgb24"))
        {
            throw ConfigError("achromatic() takes rgb24, got " + frame.pixel_format().name());
        }

        Frame output = Frame::allocate(frame.width(), frame.height(), frame.pixel_format(), frame.color(), 1);
        double spread_sum = 0.0;
        double spread_square_sum = 0.0;
        int spread_max = 0;
        for (int y = 0; y < frame.height(); ++y)
        {
            const std::uint8_t* input_row = frame.raw()->data[0] + y * frame.raw()->linesize[0];
            std::uint8_t* output_row = output.raw()->data[0] + y * output.raw()->linesize[0];
            for (int x = 0; x < frame.width(); ++x)
            {
                const std::uint32_t red = input_row[x * 3];
                const std::uint32_t green = input_row[x * 3 + 1];
                const std::uint32_t blue = input_row[x * 3 + 2];
                const auto luma =
                    static_cast<std::uint8_t>((19595U * red + 38470U * green + 7471U * blue + 32768U) >> 16);
                output_row[x * 3] = output_row[x * 3 + 1] = output_row[x * 3 + 2] = luma;

                const int spread = static_cast<int>(std::max({red, green, blue}) - std::min({red, green, blue}));
                spread_sum += spread;
                spread_square_sum += static_cast<double>(spread) * spread;
                spread_max = std::max(spread_max, spread);
            }
        }
        output.set_pts(frame.pts());
        output.set_time_base(frame.time_base());
        output.copy_embedded_from(frame);

        const double count = static_cast<double>(frame.width()) * frame.height();
        AchromaticEvidence evidence;
        evidence.channel_spread_mean = spread_sum / count;
        const double squared_deviations = std::max(0.0, spread_square_sum - spread_sum * spread_sum / count);
        evidence.channel_spread_std = count > 1.0 ? std::sqrt(squared_deviations / (count - 1.0)) : 0.0;
        evidence.channel_spread_max = spread_max;

        StageRecord record;
        record.implementation = "lossylab";
        record.input = frame.describe();
        record.transform = CoordinateTransform::identity();
        record.evidence = evidence;
        record.output = output.describe();
        record.duration_ms = clock.duration_ms();
        record.ffmpeg_duration_ms = clock.ffmpeg_duration_ms();
        return FrameResult{std::move(output), std::move(record), options};
    }
}
