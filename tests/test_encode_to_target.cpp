#include "lossylab/codec/encode.hpp"
#include "lossylab/convert/convert.hpp"
#include "lossylab/core/error.hpp"
#include "lossylab/env/capabilities.hpp"
#include "lossylab/io/decode_image.hpp"

#include <cassert>
#include <cmath>
#include <cstdint>
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

    Frame source_in(const char* pixel_format, const ColorRange range)
    {
        ColorSpec color = ColorSpec::srgb();
        color.matrix = ColorMatrix::Bt470bg;
        color.range = range;
        color.chroma_location = ChromaLocation::Center;
        const Frame rgb = decode_image(Source::from_path(data_path("testsrc_64x48.png"))).frame;
        return convert(rgb, PixelFormat::from_name(pixel_format), color).frame;
    }

    EncodeTarget target_of(const EncodeTarget::Kind kind, const double value, const double tolerance)
    {
        EncodeTarget target;
        target.kind = kind;
        target.value = value;
        target.tolerance = tolerance;
        return target;
    }

    EncodeImageOptions mjpeg_options()
    {
        EncodeImageOptions options;
        options.codec = ImageCodec::Mjpeg;
        options.pixel_format = PixelFormat::from_name("yuvj420p");
        options.rate_control = RateControl::quality(10);
        return options;
    }

    void test_an_image_is_driven_to_a_bits_per_pixel_target()
    {
        const Frame source = source_in("yuvj420p", ColorRange::Full);
        const EncodeToTargetResult result =
            encode_to_target(source, mjpeg_options(), target_of(EncodeTarget::Kind::BitsPerPixel, 3.0, 0.15));

        const EncodeSearch& search = result.search();
        assert(search.converged);
        assert(std::abs(search.achieved - 3.0) <= 0.15);
        assert(search.quality_parameter == std::round(search.quality_parameter));
        assert(!search.attempts.empty() && search.attempts.size() <= 8);
        assert(std::abs(search.achieved - static_cast<double>(result.bytes.size()) * 8.0 / (64.0 * 48.0)) < 1e-9);
        assert(search.target.kind == EncodeTarget::Kind::BitsPerPixel);

        // The configuration is the encode that won, with the quality the search
        // settled on, so replaying it needs no search.
        const EncodeImageOptions& configuration = std::get<EncodeImageOptions>(result.configuration);
        assert(configuration.rate_control.quality_parameter() == search.quality_parameter);
        assert(std::get<EncodeImageEvidence>(result.record.evidence).resolved.fixed_qscale ==
               static_cast<int>(search.quality_parameter));
    }

    void test_an_image_is_driven_to_a_psnr_target()
    {
        if (!capabilities().supports(ImageCodec::WebP))
        {
            return;
        }
        EncodeImageOptions options;
        options.codec = ImageCodec::WebP;
        options.pixel_format = PixelFormat::from_name("yuv420p");
        options.rate_control = RateControl::quality(50);

        const EncodeToTargetResult result = encode_to_target(source_in("yuv420p", ColorRange::Limited), options,
                                                             target_of(EncodeTarget::Kind::Psnr, 32.0, 0.5));
        assert(result.search().converged);
        assert(std::abs(result.search().achieved - 32.0) <= 0.5);
    }

    void test_an_unreachable_target_reports_that_it_did_not_converge()
    {
        const Frame source = source_in("yuvj420p", ColorRange::Full);
        const EncodeToTargetResult result =
            encode_to_target(source, mjpeg_options(), target_of(EncodeTarget::Kind::BitsPerPixel, 50.0, 0.1));
        assert(!result.search().converged);
        assert(!result.bytes.empty());

        // The search walked up to the finest qscale, the closest it could get.
        assert(result.search().quality_parameter == 1.0);
        assert(result.record.to_json().at("evidence").at("search").at("converged") == false);
    }

    void test_a_clip_is_driven_to_a_bits_per_pixel_target()
    {
        if (!capabilities().supports(VideoCodec::H264))
        {
            return;
        }
        std::vector<Frame> clip;
        for (int index = 0; index < 6; ++index)
        {
            Frame frame = Frame::allocate(64, 64, PixelFormat::from_name("yuv420p"), ColorSpec::bt709_limited());
            for (int plane_index = 0; plane_index < frame.plane_count(); ++plane_index)
            {
                const PlaneView plane = frame.plane(plane_index);
                for (int y = 0; y < plane.height; ++y)
                {
                    for (int x = 0; x < plane.width; ++x)
                    {
                        plane.row(y)[x] = static_cast<std::uint8_t>(16 + ((x * y + 3 * index) * 13) % 220);
                    }
                }
            }
            clip.push_back(std::move(frame));
        }

        EncodeVideoOptions options;
        options.codec = VideoCodec::H264;
        options.pixel_format = PixelFormat::from_name("yuv420p");
        options.rate_control = RateControl::crf(23);

        const EncodeToTargetResult result =
            encode_to_target(clip, options, target_of(EncodeTarget::Kind::BitsPerPixel, 1.0, 0.1));
        assert(result.search().converged);
        assert(std::abs(result.search().achieved - 1.0) <= 0.1);
        assert(result.record.kind() == StageKind::EncodeVideo);
    }

    void test_a_lossless_encode_has_nothing_to_search()
    {
        EncodeImageOptions options;
        options.codec = ImageCodec::Png;
        options.pixel_format = PixelFormat::from_name("rgb24");
        options.lossless = true;
        const Frame rgb = decode_image(Source::from_path(data_path("testsrc_64x48.png"))).frame;
        try
        {
            static_cast<void>(encode_to_target(rgb, options, target_of(EncodeTarget::Kind::BitsPerPixel, 5.0, 0.1)));
            assert(false && "expected ConfigError");
        }
        catch (const ConfigError&)
        {
        }
    }
}

int main()
{
    test_an_image_is_driven_to_a_bits_per_pixel_target();
    test_an_image_is_driven_to_a_psnr_target();
    test_an_unreachable_target_reports_that_it_did_not_converge();
    test_a_clip_is_driven_to_a_bits_per_pixel_target();
    test_a_lossless_encode_has_nothing_to_search();
    return 0;
}
