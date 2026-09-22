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

    ImageContainerInfo webp_container(const Source& source)
    {
        const ProbeResult result = probe(source);
        const StreamInfo* stream = result.primary_video_stream();
        assert(stream != nullptr);
        assert(stream->image_container.has_value());
        return *stream->image_container;
    }

    ImageContainerInfo webp_container(const char* fixture)
    {
        return webp_container(Source::from_path(data_path(fixture)));
    }

    void test_a_lossy_webp_is_reported_lossy()
    {
        const ImageContainerInfo info = webp_container("testsrc_64x48_lossy.webp");
        assert(info.compression == "lossy");
        assert(!info.has_alpha);
        assert(!info.is_animated);
        assert(info.frame_count == 1);
        assert(!info.is_still_image.has_value());
    }

    void test_a_lossless_webp_without_alpha_is_not_reported_as_alpha()
    {
        // FFmpeg decodes every lossless WebP to argb, so the pixel format
        // alone would claim alpha here.
        const ImageContainerInfo info = webp_container("testsrc_64x48_lossless.webp");
        assert(info.compression == "lossless");
        assert(!info.has_alpha);
    }

    void test_webp_alpha_is_read_from_its_chunks()
    {
        const ImageContainerInfo lossy = webp_container("testsrc_64x48_lossy_alpha.webp");
        assert(lossy.compression == "lossy");
        assert(lossy.has_alpha);
        assert(lossy.canvas_width == 64);
        assert(lossy.canvas_height == 48);

        const ImageContainerInfo lossless = webp_container("testsrc_64x48_lossless_alpha.webp");
        assert(lossless.compression == "lossless");
        assert(lossless.has_alpha);
    }

    void test_an_animated_webp_counts_its_frames()
    {
        // FFmpeg cannot open an animated WebP, so everything here comes from
        // the chunks.
        const ImageContainerInfo info = webp_container("testsrc_64x48_animated.webp");
        assert(info.is_animated);
        assert(info.frame_count == 2);
        assert(info.canvas_width == 64);
        assert(info.canvas_height == 48);
        assert(info.compression.has_value());
    }

    /// A RIFF chunk: fourcc, little-endian size, payload, padding to even.
    std::vector<std::uint8_t> riff_chunk(const std::string& fourcc, const std::vector<std::uint8_t>& payload)
    {
        std::vector<std::uint8_t> chunk(fourcc.begin(), fourcc.end());
        const auto size = static_cast<std::uint32_t>(payload.size());
        for (int shift = 0; shift < 32; shift += 8)
        {
            chunk.push_back(static_cast<std::uint8_t>(size >> shift));
        }
        chunk.insert(chunk.end(), payload.begin(), payload.end());
        if (payload.size() % 2 == 1)
        {
            chunk.push_back(0);
        }
        return chunk;
    }

    /// The image-data chunk of a simple (non-extended) WebP file: everything
    /// after the 12-byte RIFF header.
    std::vector<std::uint8_t> image_chunk_of(const char* fixture)
    {
        const std::vector<std::uint8_t> bytes = read_file(data_path(fixture));
        return {bytes.begin() + 12, bytes.end()};
    }

    void test_an_animation_mixing_lossy_and_lossless_frames_is_mixed()
    {
        // Assembled from the lossy and lossless fixtures' image chunks, each
        // wrapped in an ANMF frame at the origin.
        const std::vector<std::uint8_t> frame_header = {0, 0, 0, 0, 0, 0, 63, 0, 0, 47, 0, 0, 100, 0, 0, 0};
        std::vector<std::uint8_t> lossy_frame = frame_header;
        const std::vector<std::uint8_t> lossy_image = image_chunk_of("testsrc_64x48_lossy.webp");
        lossy_frame.insert(lossy_frame.end(), lossy_image.begin(), lossy_image.end());
        std::vector<std::uint8_t> lossless_frame = frame_header;
        const std::vector<std::uint8_t> lossless_image = image_chunk_of("testsrc_64x48_lossless.webp");
        lossless_frame.insert(lossless_frame.end(), lossless_image.begin(), lossless_image.end());

        std::vector<std::uint8_t> body = {'W', 'E', 'B', 'P'};
        for (const std::vector<std::uint8_t>& chunk :
             {riff_chunk("VP8X", {0x02, 0, 0, 0, 63, 0, 0, 47, 0, 0}), riff_chunk("ANIM", {0, 0, 0, 0, 0, 0}),
              riff_chunk("ANMF", lossy_frame), riff_chunk("ANMF", lossless_frame)})
        {
            body.insert(body.end(), chunk.begin(), chunk.end());
        }
        std::vector<std::uint8_t> file = riff_chunk("RIFF", body);

        const ImageContainerInfo info = webp_container(Source::from_bytes(std::move(file), "webp"));
        assert(info.compression == "mixed");
        assert(info.frame_count == 2);
    }

    void test_webp_container_info_from_memory_matches_path()
    {
        const ImageContainerInfo from_path = webp_container("testsrc_64x48_lossy_alpha.webp");
        const ImageContainerInfo from_memory =
            webp_container(Source::from_bytes(read_file(data_path("testsrc_64x48_lossy_alpha.webp"))));
        assert(from_path.to_json() == from_memory.to_json());
    }

    // AVIF fixtures made by avifenc 1.0.4 from a 128x96 testsrc frame: a single
    // image with an alpha auxiliary image, and 2x2 grids of 64x64 tiles with
    // and without an alpha grid.
    const char* avif_alpha_fixture = "testsrc_128x96_alpha.avif";
    const char* avif_grid_fixture = "testsrc_128x96_grid.avif";
    const char* avif_grid_alpha_fixture = "testsrc_128x96_grid_alpha.avif";

    void test_a_grid_image_reports_its_tile_grid()
    {
        const ProbeResult result = probe(Source::from_path(data_path(avif_grid_fixture)));
        assert(result.major_brand == "avif");
        assert(result.tile_grids.size() == std::size_t{1});

        const TileGrid* grid = result.primary_tile_grid();
        assert(grid != nullptr);
        assert(grid->title == "Color");
        assert(grid->width == 128);
        assert(grid->height == 96);
        assert(grid->tiles.size() == std::size_t{4});
        for (const TileGrid::Tile& tile : grid->tiles)
        {
            assert(result.streams[static_cast<std::size_t>(tile.stream_index)].is_dependent);
            assert(tile.x == 0 || tile.x == 64);
            assert(tile.y == 0 || tile.y == 64);
        }
        assert(grid->tiles[3].x == 64 && grid->tiles[3].y == 64);
    }

    void test_a_grid_image_has_no_primary_stream()
    {
        // Every stream is one tile, so none of them is the picture.
        const ProbeResult result = probe(Source::from_path(data_path(avif_grid_fixture)));
        assert(result.primary_video_stream() == nullptr);
        assert(result.additional_images().stream_indices.empty());
        assert(result.additional_images().tile_grid_ids.empty());
    }

    void test_an_alpha_grid_is_an_additional_image()
    {
        const ProbeResult result = probe(Source::from_path(data_path(avif_grid_alpha_fixture)));
        assert(result.tile_grids.size() == std::size_t{2});
        assert(result.primary_tile_grid()->title == "Color");

        const ProbeResult::AdditionalImages additional = result.additional_images();
        assert(additional.stream_indices.empty());
        assert(additional.tile_grid_ids.size() == std::size_t{1});
        const TileGrid& alpha = result.tile_grids[1];
        assert(alpha.id == additional.tile_grid_ids[0]);
        assert(!alpha.is_primary);
        assert(alpha.title == "Alpha");
    }

    void test_a_single_image_alpha_plane_is_an_additional_image()
    {
        const ProbeResult result = probe(Source::from_path(data_path(avif_alpha_fixture)));
        const StreamInfo* primary = result.primary_video_stream();
        assert(primary != nullptr);
        assert(primary->index == 0);
        assert(primary->is_default);
        assert(primary->image_container->is_still_image == true);

        const ProbeResult::AdditionalImages additional = result.additional_images();
        assert(additional.stream_indices == std::vector<int>{1});
        assert(result.streams[1].metadata.at("title") == "Alpha");
        assert(result.tile_grids.empty());
    }

    void test_a_video_has_no_additional_images()
    {
        const ProbeResult result = probe(Source::from_path(data_path(video_fixture)));
        assert(result.primary_video_stream()->is_default);
        assert(result.additional_images().stream_indices.empty());
    }

    void test_decode_image_refuses_a_grid_rather_than_returning_a_tile()
    {
        try
        {
            static_cast<void>(decode_image(Source::from_path(data_path(avif_grid_fixture))));
            assert(false && "expected NotImplemented");
        }
        catch (const NotImplemented&)
        {
        }
    }

    void test_an_mp4_reports_its_brands_and_encoder()
    {
        const ProbeResult result = probe(Source::from_path(data_path(video_fixture)));

        // Brands and encoder strings are the cheapest strong evidence of which tool
        // produced a file, so they must come through verbatim.
        assert(result.major_brand == "isom");
        assert(result.compatible_brands == (std::vector<std::string>{"isom", "iso2", "avc1", "mp41"}));

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
    test_a_lossy_webp_is_reported_lossy();
    test_a_lossless_webp_without_alpha_is_not_reported_as_alpha();
    test_webp_alpha_is_read_from_its_chunks();
    test_an_animated_webp_counts_its_frames();
    test_an_animation_mixing_lossy_and_lossless_frames_is_mixed();
    test_webp_container_info_from_memory_matches_path();
    test_a_grid_image_reports_its_tile_grid();
    test_a_grid_image_has_no_primary_stream();
    test_an_alpha_grid_is_an_additional_image();
    test_a_single_image_alpha_plane_is_an_additional_image();
    test_a_video_has_no_additional_images();
    test_decode_image_refuses_a_grid_rather_than_returning_a_tile();
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
