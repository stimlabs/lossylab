#include "lossylab/core/error.hpp"
#include "lossylab/core/schema_version.hpp"
#include "lossylab/io/decode_image.hpp"
#include "lossylab/io/probe.hpp"

#include <cassert>
#include <cstdio>
#include <optional>
#include <vector>

using namespace lossylab;

namespace
{
    std::string data_path(std::string_view name)
    {
        return std::string(LOSSYLAB_TEST_DATA_DIR) + "/" + std::string(name);
    }

    std::vector<std::uint8_t> read_file(const std::string& path)
    {
        std::FILE* file = std::fopen(path.c_str(), "rb");
        assert(file != nullptr && "could not open fixture");
        std::vector<std::uint8_t> bytes;
        std::uint8_t buffer[4096];
        std::size_t read = 0;
        while ((read = std::fread(buffer, 1, sizeof(buffer), file)) > 0)
        {
            bytes.insert(bytes.end(), buffer, buffer + read);
        }
        std::fclose(file);
        return bytes;
    }

    const char* png_fixture = "testsrc_64x48.png";
    const char* jpeg_fixture = "testsrc_64x48.jpg";
    const char* video_fixture = "testsrc_64x48.mp4";

    void test_probing_a_png_reports_its_geometry()
    {
        const ProbeResult result = probe(Source::from_path(data_path(png_fixture)));

        const StreamInfo* video = result.primary_video_stream();
        assert(video != nullptr);
        assert(video->codec_name == std::string("png"));
        assert(video->width == 64);
        assert(video->height == 48);
        assert(video->bit_depth == 8);
        assert(video->pixel_format.is_rgb());
    }

    void test_probing_a_jpeg_reports_chroma_and_range()
    {
        const ProbeResult result = probe(Source::from_path(data_path(jpeg_fixture)));

        const StreamInfo* video = result.primary_video_stream();
        assert(video != nullptr);
        assert(video->codec_name == std::string("mjpeg"));
        assert(video->width == 64);

        // JPEG is 4:2:0 full range here, which is exactly the kind of property the
        // audit path reads without decoding.
        assert(video->pixel_format.subsampling() == Subsampling::Yuv420);
        assert(video->color.range == ColorRange::Full);
    }

    void test_probing_a_video_reports_codec_profile_and_tags()
    {
        const ProbeResult result = probe(Source::from_path(data_path(video_fixture)));

        const StreamInfo* video = result.primary_video_stream();
        assert(video != nullptr);
        assert(video->codec_name == std::string("h264"));
        assert(video->width == 64);
        assert(video->height == 48);

        // Profile and level identify the encoder configuration.
        assert(!video->profile.empty());
        assert(video->level.has_value());

        // The fixture was tagged BT.709 explicitly, so the tags must survive.
        assert(video->color.matrix == ColorMatrix::Bt709);
        assert(video->color.primaries == ColorPrimaries::Bt709);

        assert(video->frame_rate.is_valid());
        assert(result.duration_us.has_value());
    }

    void test_a_path_extension_mismatch_is_reported()
    {
        // Bytes are a PNG throughout; the path just claims a different
        // extension, as a curator's renamed or re-wrapped file would.
        const std::vector<std::uint8_t> bytes = read_file(data_path(png_fixture));

        const ProbeResult matching = probe(Source::from_memory(bytes, "png"));
        assert(matching.claimed_extension == std::string("png"));
        assert(!matching.format_mismatch);

        const ProbeResult mismatched = probe(Source::from_memory(bytes, "mp4"));
        assert(mismatched.claimed_extension == std::string("mp4"));
        assert(mismatched.format_mismatch);
    }

    void test_a_source_with_no_extension_is_never_a_mismatch()
    {
        const ProbeResult result = probe(Source::from_memory(read_file(data_path(png_fixture))));
        assert(result.claimed_extension.empty());
        assert(!result.format_mismatch);
    }

    void test_a_constant_frame_rate_video_is_not_flagged_variable()
    {
        const ProbeResult result = probe(Source::from_path(data_path(video_fixture)));
        const StreamInfo* video = result.primary_video_stream();
        assert(video != nullptr);
        assert(!video->is_variable_frame_rate);
    }

    void test_probe_result_carries_the_schema_version()
    {
        const ProbeResult result = probe(Source::from_path(data_path(video_fixture)));
        assert(result.to_json().at("schema_version").get<int>() == schema_version);
    }

    void test_non_image_streams_have_no_image_container_info()
    {
        const ProbeResult result = probe(Source::from_path(data_path(video_fixture)));
        const StreamInfo* video = result.primary_video_stream();
        assert(video != nullptr);
        assert(!video->image_container.has_value());
    }

    void test_an_mp4_reports_its_brands_and_encoder()
    {
        const ProbeResult result = probe(Source::from_path(data_path(video_fixture)));

        // Brands and encoder strings are the cheapest strong evidence of which tool
        // produced a file, so they must come through verbatim.
        assert(!result.major_brand.empty());
        assert(!result.compatible_brands.empty());

        const std::optional<std::string> encoder = result.encoder_string();
        assert(encoder.has_value());
        assert(!encoder->empty());
    }

    void test_probing_from_memory_matches_probing_from_a_path()
    {
        // A dataloader usually has the bytes already and should not need a
        // temporary file to hand them over.
        const std::vector<std::uint8_t> bytes = read_file(data_path(png_fixture));

        const ProbeResult from_path = probe(Source::from_path(data_path(png_fixture)));
        const ProbeResult from_memory = probe(Source::from_memory(bytes));

        const StreamInfo* a = from_path.primary_video_stream();
        const StreamInfo* b = from_memory.primary_video_stream();
        assert(a != nullptr && b != nullptr);
        assert(a->width == b->width);
        assert(a->height == b->height);
        assert(a->codec_name == b->codec_name);
        assert(a->pixel_format == b->pixel_format);
    }

    void test_an_owning_memory_source_keeps_its_bytes_alive()
    {
        const Source source = Source::from_bytes(read_file(data_path(png_fixture)));
        const ProbeResult result = probe(source);
        assert(result.primary_video_stream() != nullptr);
    }

    void test_a_copied_owning_source_outlives_the_original()
    {
        std::optional<Source> original = Source::from_bytes(read_file(data_path(png_fixture)));
        const Source copy = *original;
        original.reset();
        assert(copy.bytes().size() == read_file(data_path(png_fixture)).size());
        assert(probe(copy).primary_video_stream() != nullptr);
    }

    void test_probe_failures_are_reported_not_guessed()
    {
        try
        {
            (void)probe(Source::from_path("/nonexistent/path/to/nothing.png"));
            assert(false && "expected throw");
        }
        catch (const Error&)
        {
        }

        // Bytes that are not a media file at all.
        const std::vector<std::uint8_t> garbage(512, 0x7F);
        try
        {
            (void)probe(Source::from_memory(garbage));
            assert(false && "expected throw");
        }
        catch (const Error&)
        {
        }

        try
        {
            (void)Source::from_path("");
            assert(false && "expected throw");
        }
        catch (const ConfigError&)
        {
        }

        try
        {
            (void)probe(Source::from_memory({}));
            assert(false && "expected throw");
        }
        catch (const ConfigError&)
        {
        }
    }

    void test_probe_serializes()
    {
        const ProbeResult result = probe(Source::from_path(data_path(video_fixture)));
        const json::Value document = result.to_json();

        assert(!document.at("format").get<std::string>().empty());
        assert(document.at("streams").size() >= 1);
        assert(json::parse(document.dump()) == document);
    }

    // -----------------------------------------------------------------------
    // decode_image
    // -----------------------------------------------------------------------

    void test_decoding_a_png_gives_native_planes()
    {
        const FrameResult result = decode_image(Source::from_path(data_path(png_fixture)));

        assert(result.frame.width() == 64);
        assert(result.frame.height() == 48);
        assert(result.frame.pixel_format().is_rgb());

        // Native format by default: an audit wants the planes as the encoder wrote
        // them, not an RGB rendering chosen on its behalf.
        assert(result.record.kind == StageKind::Decode);
        assert(result.record.implementation == std::string("png"));
        assert(result.record.transform.is_identity());
    }

    void test_decoding_a_jpeg_keeps_its_chroma_subsampling()
    {
        const FrameResult result = decode_image(Source::from_path(data_path(jpeg_fixture)));

        // The whole point of native-plane access: the 4:2:0 chroma is still 4:2:0,
        // available for inspection rather than already upsampled away.
        assert(result.frame.pixel_format().subsampling() == Subsampling::Yuv420);
        assert(result.frame.plane(1).width == 32);
        assert(result.record.implementation == std::string("mjpeg"));
    }

    void test_an_untagged_file_records_the_assumption_made_for_it()
    {
        // PNG carries no color tags. Assuming something is unavoidable; doing it
        // silently is not, so the assumption has to appear in the record.
        const FrameResult result = decode_image(Source::from_path(data_path(png_fixture)));

        assert(result.frame.color().is_fully_specified());

        bool recorded = false;
        for (const ConversionEvent& conversion : result.record.conversions)
        {
            if (conversion.property == "color_tags")
            {
                recorded = true;
            }
        }
        assert(recorded);
        assert(!result.record.params.at("color_fully_tagged").get<bool>());
    }

    void test_decoding_to_an_explicit_format_records_the_conversion()
    {
        DecodeImageOptions options;
        options.pixel_format = PixelFormat::from_name("yuv420p");
        options.color = ColorSpec::bt709_limited();

        const FrameResult result = decode_image(Source::from_path(data_path(png_fixture)), options);

        assert(result.frame.pixel_format().name() == std::string("yuv420p"));
        assert(result.frame.color() == ColorSpec::bt709_limited());

        // The record's output has to describe what actually came back.
        assert(result.record.output.pixel_format == result.frame.pixel_format());
        assert(result.record.output.color == result.frame.color());

        bool format_change_recorded = false;
        for (const ConversionEvent& conversion : result.record.conversions)
        {
            if (conversion.property == "pix_fmt")
            {
                format_change_recorded = true;
            }
        }
        assert(format_change_recorded);
    }

    void test_decoding_from_memory_matches_decoding_from_a_path()
    {
        const std::vector<std::uint8_t> bytes = read_file(data_path(jpeg_fixture));

        const FrameResult from_path = decode_image(Source::from_path(data_path(jpeg_fixture)));
        const FrameResult from_memory = decode_image(Source::from_memory(bytes));

        assert(from_path.frame.width() == from_memory.frame.width());
        assert(from_path.frame.pixel_format() == from_memory.frame.pixel_format());

        // Same bytes, same samples: the two paths must not differ at all.
        const ConstPlaneView a = from_path.frame.plane(0);
        const ConstPlaneView b = from_memory.frame.plane(0);
        for (int y = 0; y < a.height; ++y)
        {
            for (int x = 0; x < a.width; ++x)
            {
                assert(a.row(y)[x] == b.row(y)[x]);
            }
        }
    }

    void test_decoding_is_deterministic()
    {
        // Thread counts are pinned, so two decodes of the same bytes agree exactly.
        const FrameResult first = decode_image(Source::from_path(data_path(jpeg_fixture)));
        const FrameResult second = decode_image(Source::from_path(data_path(jpeg_fixture)));

        const ConstPlaneView a = first.frame.plane(0);
        const ConstPlaneView b = second.frame.plane(0);
        for (int y = 0; y < a.height; ++y)
        {
            for (int x = 0; x < a.width; ++x)
            {
                assert(a.row(y)[x] == b.row(y)[x]);
            }
        }
    }
}

int main()
{
    test_probing_a_png_reports_its_geometry();
    test_probing_a_jpeg_reports_chroma_and_range();
    test_probing_a_video_reports_codec_profile_and_tags();
    test_an_mp4_reports_its_brands_and_encoder();
    test_a_path_extension_mismatch_is_reported();
    test_a_source_with_no_extension_is_never_a_mismatch();
    test_a_constant_frame_rate_video_is_not_flagged_variable();
    test_probe_result_carries_the_schema_version();
    test_non_image_streams_have_no_image_container_info();
    test_probing_from_memory_matches_probing_from_a_path();
    test_an_owning_memory_source_keeps_its_bytes_alive();
    test_a_copied_owning_source_outlives_the_original();
    test_probe_failures_are_reported_not_guessed();
    test_probe_serializes();
    test_decoding_a_png_gives_native_planes();
    test_decoding_a_jpeg_keeps_its_chroma_subsampling();
    test_an_untagged_file_records_the_assumption_made_for_it();
    test_decoding_to_an_explicit_format_records_the_conversion();
    test_decoding_from_memory_matches_decoding_from_a_path();
    test_decoding_is_deterministic();
}
