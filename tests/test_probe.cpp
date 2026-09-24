#include "lossylab/convert/convert.hpp"
#include "lossylab/core/error.hpp"
#include "lossylab/core/schema_version.hpp"
#include "lossylab/io/decode_image.hpp"
#include "lossylab/io/probe.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>
#include <filesystem>
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

    void test_a_jpeg_is_not_a_mismatch_under_any_of_its_extensions()
    {
        // The two fixtures are read by different demuxers ("jpeg_pipe" and "image2"), neither of which lists
        // extensions, so the codec decides.
        for (const char* fixture : {jpeg_fixture, "testsrc_64x48_422_orientation6.jpg"})
        {
            const std::vector<std::uint8_t> bytes = read_file(data_path(fixture));
            for (const char* extension : {"jpg", "jpeg", "jpe", "jfif"})
            {
                assert(!probe(Source::from_memory(bytes, extension)).format_mismatch);
            }
            assert(probe(Source::from_memory(bytes, "png")).format_mismatch);
            assert(probe(Source::from_memory(bytes, "mp4")).format_mismatch);
        }
    }

    void test_a_still_image_is_not_a_mismatch_under_its_codec_extensions()
    {
        // Each of these is read by a "*_pipe" demuxer named differently from the extension (tiff_pipe, j2k_pipe,
        // jpegxl_pipe), so the codec decides.
        struct Case
        {
            const char* fixture;
            std::vector<const char*> extensions;
        };
        const std::vector<Case> cases = {
            {"testsrc_64x48.tif", {"tif", "tiff"}},
            {"testsrc_64x48.jp2", {"jp2", "j2k", "jpx"}},
            {"testsrc_64x48.j2k", {"jp2", "j2k", "jpx"}},
            {"testsrc_64x48.jxl", {"jxl"}},
        };
        for (const Case& test_case : cases)
        {
            const std::vector<std::uint8_t> bytes = read_file(data_path(test_case.fixture));
            for (const char* extension : test_case.extensions)
            {
                assert(!probe(Source::from_memory(bytes, extension)).format_mismatch);
            }
            assert(probe(Source::from_memory(bytes, "jpg")).format_mismatch);
            assert(probe(Source::from_memory(bytes, "mp4")).format_mismatch);
        }
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

    void test_deeply_nested_animation_frames_are_not_walked_into()
    {
        // ANMF chunks nested far deeper than the stack could follow; the
        // format allows ANMF only at the top level. Built outside-in, with
        // each chunk's size computed from the innermost one outwards.
        constexpr std::size_t depth = 300000;
        constexpr std::uint32_t nested_header_size = 8 + 16;
        const std::vector<std::uint8_t> innermost = image_chunk_of("testsrc_64x48_lossy.webp");

        std::vector<std::uint8_t> body = {'W', 'E', 'B', 'P'};
        const std::vector<std::uint8_t> extended = riff_chunk("VP8X", {0x02, 0, 0, 0, 63, 0, 0, 47, 0, 0});
        body.insert(body.end(), extended.begin(), extended.end());
        body.reserve(body.size() + depth * nested_header_size + innermost.size());
        for (std::size_t level = 0; level < depth; ++level)
        {
            const auto payload_size =
                static_cast<std::uint32_t>(16 + (depth - level - 1) * nested_header_size + innermost.size());
            body.insert(body.end(), {'A', 'N', 'M', 'F'});
            for (int shift = 0; shift < 32; shift += 8)
            {
                body.push_back(static_cast<std::uint8_t>(payload_size >> shift));
            }
            body.insert(body.end(), 16, 0);
        }
        body.insert(body.end(), innermost.begin(), innermost.end());
        std::vector<std::uint8_t> file = riff_chunk("RIFF", body);

        const ImageContainerInfo info = webp_container(Source::from_bytes(std::move(file), "webp"));
        assert(info.is_animated);
        assert(info.frame_count == 1);
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

    void test_a_grid_images_rotation_is_reported_on_the_grid()
    {
        // irot 3 is three quarter turns anticlockwise: EXIF orientation 6. An
        // iPhone photo taken in portrait carries it on the grid, not on a stream.
        const ProbeResult result = probe(Source::from_path(data_path("testsrc_128x96_grid_irot3.avif")));
        const TileGrid* grid = result.primary_tile_grid();
        assert(grid != nullptr);
        assert(grid->orientation == 6);
        assert(grid->to_json().at("orientation") == 6);

        assert(!probe(Source::from_path(data_path(avif_grid_fixture))).primary_tile_grid()->orientation.has_value());
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

    void test_decoding_a_single_image_records_no_tile_grid()
    {
        const DecodedImage result = decode_image(Source::from_path(data_path(png_fixture)));
        assert(result.record.params.at("tile_grid_id").is_null());
        assert(result.tile_grid() == nullptr);
        assert(&result.stream() == &result.probe.streams.front());
    }

    void test_decoding_reports_what_probing_reports()
    {
        for (const char* name : {png_fixture, jpeg_fixture})
        {
            const Source source = Source::from_path(data_path(name));
            assert(decode_image(source).probe.to_json() == probe(source).to_json());
        }
    }

    // JPEG fixtures written by Pillow 12.3 (libjpeg-turbo) from the 64x48
    // testsrc frame; the name says quality and options. The older
    // testsrc_64x48.jpg was written by FFmpeg's own JPEG encoder.
    JpegInfo jpeg_info(const Source& source)
    {
        const ProbeResult result = probe(source);
        const StreamInfo* stream = result.primary_video_stream();
        assert(stream != nullptr);
        assert(stream->jpeg.has_value());
        return *stream->jpeg;
    }

    JpegInfo jpeg_info(const char* fixture)
    {
        return jpeg_info(Source::from_path(data_path(fixture)));
    }

    void test_a_libjpeg_baseline_jpeg_is_recognized_with_its_quality()
    {
        const JpegInfo info = jpeg_info("testsrc_64x48_q75.jpg");
        assert(info.process == "baseline");
        assert(!info.arithmetic_coding);
        assert(info.precision == 8);
        assert(info.ijg_quality == 75);
        assert(info.ijg_quality_exact);
        assert(info.huffman_tables == "standard");
        assert(info.scan_count == 1);
        assert(info.restart_interval == 0);

        // 4:2:0: the luma component samples twice as densely both ways.
        assert(info.components.size() == std::size_t{3});
        assert(info.components[0].horizontal_sampling == 2 && info.components[0].vertical_sampling == 2);
        assert(info.components[1].horizontal_sampling == 1 && info.components[1].vertical_sampling == 1);
        assert(info.components[0].quantization_table == 0);
        assert(info.components[1].quantization_table == 1);

        // Quality 75 scales the standard tables by half: 16 -> 8, 99 -> 50.
        assert(info.quantization_tables.size() == std::size_t{2});
        assert(info.quantization_tables[0].values[0] == 8);
        assert(info.quantization_tables[0].values[63] == 50);

        assert(info.segments.size() == std::size_t{1});
        assert(info.segments[0].marker == "APP0");
        assert(info.segments[0].identifier == "JFIF");
        assert(!info.comment.has_value());
        assert(info.has_end_of_image);
        assert(info.trailing_bytes == 0);
    }

    void test_optimized_huffman_tables_are_custom()
    {
        const JpegInfo info = jpeg_info("testsrc_64x48_q90_optimized_444.jpg");
        assert(info.ijg_quality == 90);
        assert(info.ijg_quality_exact);
        assert(info.huffman_tables == "custom");
        for (const JpegInfo::Component& component : info.components)
        {
            assert(component.horizontal_sampling == 1 && component.vertical_sampling == 1);
        }
        assert(info.comment == "lossylab test fixture");
    }

    void test_a_progressive_jpeg_reports_its_scans()
    {
        const JpegInfo info = jpeg_info("testsrc_64x48_q85_progressive.jpg");
        assert(info.process == "progressive");
        assert(info.scan_count > 1);
        assert(info.huffman_tables == "custom");
        assert(info.ijg_quality == 85);
        assert(info.ijg_quality_exact);
    }

    void test_a_grayscale_jpeg_uses_one_table()
    {
        const JpegInfo info = jpeg_info("testsrc_64x48_q50_gray.jpg");
        assert(info.components.size() == std::size_t{1});
        assert(info.ijg_quality == 50);
        assert(info.ijg_quality_exact);
        assert(info.huffman_tables == "standard");
    }

    void test_a_cmyk_jpeg_reports_its_adobe_transform()
    {
        const JpegInfo info = jpeg_info("testsrc_64x48_q80_cmyk.jpg");
        assert(info.components.size() == std::size_t{4});
        assert(info.adobe_transform == 0);
        assert(std::any_of(info.segments.begin(), info.segments.end(),
                           [](const JpegInfo::Segment& segment)
                           { return segment.marker == "APP14" && segment.identifier.starts_with("Adobe"); }));
        assert(info.ijg_quality == 80);
    }

    void test_another_encoders_tables_are_not_an_exact_libjpeg_match()
    {
        const JpegInfo info = jpeg_info(jpeg_fixture);
        assert(info.comment.has_value() && info.comment->starts_with("Lavc"));
        assert(info.ijg_quality.has_value());
        assert(!info.ijg_quality_exact);
    }

    void test_a_truncated_jpeg_has_no_end_of_image()
    {
        std::vector<std::uint8_t> bytes = read_file(data_path("testsrc_64x48_q75.jpg"));
        bytes.resize(bytes.size() - 10);
        const JpegInfo info = jpeg_info(Source::from_bytes(std::move(bytes)));
        assert(!info.has_end_of_image);
        assert(info.ijg_quality == 75);
    }

    void test_bytes_after_the_end_of_image_are_counted()
    {
        std::vector<std::uint8_t> bytes = read_file(data_path("testsrc_64x48_q75.jpg"));
        bytes.insert(bytes.end(), 100, 0xab);
        const JpegInfo info = jpeg_info(Source::from_bytes(std::move(bytes)));
        assert(info.has_end_of_image);
        assert(info.trailing_bytes == 100);
    }

    void test_a_damaged_segment_length_stops_the_walk_without_failing()
    {
        // The DQT segment's length is overwritten with one that runs past the
        // end of the file.
        std::vector<std::uint8_t> bytes = read_file(data_path("testsrc_64x48_q75.jpg"));
        const std::array<std::uint8_t, 2> dqt_marker = {0xff, 0xdb};
        const auto dqt = std::search(bytes.begin(), bytes.end(), dqt_marker.begin(), dqt_marker.end());
        assert(dqt != bytes.end());
        dqt[2] = 0xff;
        dqt[3] = 0xff;
        const JpegInfo info = jpeg_info(Source::from_bytes(std::move(bytes)));
        assert(!info.has_end_of_image);
    }

    void test_jpeg_markers_from_memory_match_path()
    {
        const JpegInfo from_path = jpeg_info("testsrc_64x48_q85_progressive.jpg");
        const JpegInfo from_memory =
            jpeg_info(Source::from_bytes(read_file(data_path("testsrc_64x48_q85_progressive.jpg"))));
        assert(from_path.to_json() == from_memory.to_json());
    }

    void test_non_jpeg_streams_have_no_jpeg_info()
    {
        assert(!probe(Source::from_path(data_path(png_fixture))).primary_video_stream()->jpeg.has_value());
        assert(!probe(Source::from_path(data_path(video_fixture))).primary_video_stream()->jpeg.has_value());
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

        assert(!document.at("format_name").get<std::string>().empty());
        assert(document.at("streams").size() >= 1);
        assert(document.at("streams").at(0).contains("codec_name"));
        assert(document.at("streams").at(0).contains("pixel_format"));
        assert(json::parse(document.dump()) == document);
    }

    void test_a_decoded_image_starts_a_processing_history()
    {
        const DecodedImage decoded = decode_image(Source::from_path(data_path(jpeg_fixture)));
        ProcessingRecord history = decoded.processing_record();
        assert(history.origin().has_value());
        assert(history.origin()->to_json() == decoded.probe.to_json());
        assert(history.size() == 1 && history.stages().front().kind == StageKind::Decode);

        ConvertOptions options;
        options.pixel_format = PixelFormat::from_name("yuv444p");
        options.color = decoded.frame.color();
        history.append(convert(decoded.frame, options).record);
        history.validate_continuity();

        const ProcessingRecord read_back = ProcessingRecord::from_json(history.to_json());
        assert(read_back.to_json() == history.to_json());
        assert(read_back.origin()->streams.front().jpeg.has_value());
        assert(!history.to_json().at("origin").contains("schema_version"));

        assert(ProcessingRecord().to_json().at("origin").is_null());
    }

    void test_every_fixture_reads_back_from_json()
    {
        for (const std::filesystem::directory_entry& entry :
             std::filesystem::directory_iterator(std::string(LOSSYLAB_TEST_DATA_DIR)))
        {
            const std::string extension = entry.path().extension().string();
            if (!entry.is_regular_file() || extension == ".csv" || extension == ".py" || extension == ".y4m")
            {
                continue;
            }
            const json::Value written = probe(Source::from_path(entry.path().string())).to_json();
            assert(ProbeResult::from_json(written).to_json() == written);
        }
    }

    // -----------------------------------------------------------------------
    // decode_image
    // -----------------------------------------------------------------------

    void test_decoding_a_png_gives_native_planes()
    {
        const DecodedImage result = decode_image(Source::from_path(data_path(png_fixture)));

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
        const DecodedImage result = decode_image(Source::from_path(data_path(jpeg_fixture)));

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
        const DecodedImage result = decode_image(Source::from_path(data_path(png_fixture)));

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
        assert(!result.stream().color_fully_tagged);
    }

    void test_decoding_to_an_explicit_format_records_the_conversion()
    {
        // BT.709 YUV with the sRGB transfer the untagged PNG is assumed to
        // have, since convert() cannot change the transfer.
        ColorSpec target = ColorSpec::bt709_limited();
        target.transfer = TransferCharacteristic::Srgb;

        DecodeImageOptions options;
        options.pixel_format = PixelFormat::from_name("yuv420p");
        options.color = target;

        const DecodedImage result = decode_image(Source::from_path(data_path(png_fixture)), options);

        assert(result.frame.pixel_format().name() == std::string("yuv420p"));
        assert(result.frame.color() == target);

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

        const DecodedImage from_path = decode_image(Source::from_path(data_path(jpeg_fixture)));
        const DecodedImage from_memory = decode_image(Source::from_memory(bytes));

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
        const DecodedImage first = decode_image(Source::from_path(data_path(jpeg_fixture)));
        const DecodedImage second = decode_image(Source::from_path(data_path(jpeg_fixture)));

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
    test_a_jpeg_is_not_a_mismatch_under_any_of_its_extensions();
    test_a_still_image_is_not_a_mismatch_under_its_codec_extensions();
    test_a_constant_frame_rate_video_is_not_flagged_variable();
    test_probe_result_carries_the_schema_version();
    test_non_image_streams_have_no_image_container_info();
    test_a_lossy_webp_is_reported_lossy();
    test_a_lossless_webp_without_alpha_is_not_reported_as_alpha();
    test_webp_alpha_is_read_from_its_chunks();
    test_an_animated_webp_counts_its_frames();
    test_an_animation_mixing_lossy_and_lossless_frames_is_mixed();
    test_deeply_nested_animation_frames_are_not_walked_into();
    test_webp_container_info_from_memory_matches_path();
    test_a_libjpeg_baseline_jpeg_is_recognized_with_its_quality();
    test_optimized_huffman_tables_are_custom();
    test_a_progressive_jpeg_reports_its_scans();
    test_a_grayscale_jpeg_uses_one_table();
    test_a_cmyk_jpeg_reports_its_adobe_transform();
    test_another_encoders_tables_are_not_an_exact_libjpeg_match();
    test_a_truncated_jpeg_has_no_end_of_image();
    test_bytes_after_the_end_of_image_are_counted();
    test_a_damaged_segment_length_stops_the_walk_without_failing();
    test_jpeg_markers_from_memory_match_path();
    test_non_jpeg_streams_have_no_jpeg_info();
    test_a_grid_image_reports_its_tile_grid();
    test_a_grid_image_has_no_primary_stream();
    test_a_grid_images_rotation_is_reported_on_the_grid();
    test_an_alpha_grid_is_an_additional_image();
    test_a_single_image_alpha_plane_is_an_additional_image();
    test_a_video_has_no_additional_images();
    test_decoding_a_single_image_records_no_tile_grid();
    test_decoding_reports_what_probing_reports();
    test_probing_from_memory_matches_probing_from_a_path();
    test_an_owning_memory_source_keeps_its_bytes_alive();
    test_a_copied_owning_source_outlives_the_original();
    test_probe_failures_are_reported_not_guessed();
    test_probe_serializes();
    test_every_fixture_reads_back_from_json();
    test_a_decoded_image_starts_a_processing_history();
    test_decoding_a_png_gives_native_planes();
    test_decoding_a_jpeg_keeps_its_chroma_subsampling();
    test_an_untagged_file_records_the_assumption_made_for_it();
    test_decoding_to_an_explicit_format_records_the_conversion();
    test_decoding_from_memory_matches_decoding_from_a_path();
    test_decoding_is_deterministic();
}
