#include "lossylab/core/error.hpp"
#include "lossylab/env/capabilities.hpp"
#include "lossylab/io/decode_image.hpp"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <string>
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

    // Grids made by avifenc 1.0.4 from a 128x96 testsrc frame: 2x2 grids of
    // 64x64 tiles, so a 128x128 canvas cropped to 128x96. The references are
    // avifdec's decode of the same files (dav1d, YUV kept as coded) as y4m.
    const char* grid_444_fixture = "testsrc_128x96_grid.avif";
    const char* grid_444_reference = "testsrc_128x96_grid_reference.y4m";
    const char* grid_420_fixture = "testsrc_128x96_grid_420.avif";
    const char* grid_420_reference = "testsrc_128x96_grid_420_reference.y4m";
    const char* grid_alpha_fixture = "testsrc_128x96_grid_alpha.avif";

    /// The planes of a single-frame 8-bit y4m file.
    struct Y4mImage
    {
        int width = 0;
        int height = 0;
        int chroma_width = 0;
        int chroma_height = 0;
        std::vector<std::vector<std::uint8_t>> planes;
    };

    std::string header_token(const std::string& header, const char tag)
    {
        const std::size_t start = header.find(std::string(" ") + tag);
        assert(start != std::string::npos && "y4m header lacks a field");
        const std::size_t end = header.find(' ', start + 1);
        return header.substr(start + 2, end == std::string::npos ? std::string::npos : end - start - 2);
    }

    Y4mImage read_y4m(const char* fixture)
    {
        const std::vector<std::uint8_t> bytes = read_file(data_path(fixture));
        const auto header_end = std::find(bytes.begin(), bytes.end(), std::uint8_t{'\n'});
        const std::string header(bytes.begin(), header_end);

        Y4mImage image;
        image.width = std::stoi(header_token(header, 'W'));
        image.height = std::stoi(header_token(header, 'H'));
        const std::string chroma = header_token(header, 'C');
        const bool subsampled = chroma.starts_with("420");
        image.chroma_width = subsampled ? (image.width + 1) / 2 : image.width;
        image.chroma_height = subsampled ? (image.height + 1) / 2 : image.height;

        // "FRAME" and its parameters end at the next newline.
        auto data = std::find(header_end + 1, bytes.end(), std::uint8_t{'\n'}) + 1;
        const std::size_t luma_size = static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height);
        const std::size_t chroma_size =
            static_cast<std::size_t>(image.chroma_width) * static_cast<std::size_t>(image.chroma_height);
        for (const std::size_t size : {luma_size, chroma_size, chroma_size})
        {
            assert(bytes.end() - data >= static_cast<std::ptrdiff_t>(size));
            image.planes.emplace_back(data, data + static_cast<std::ptrdiff_t>(size));
            data += static_cast<std::ptrdiff_t>(size);
        }
        return image;
    }

    void assert_matches_reference(const Frame& frame, const Y4mImage& reference)
    {
        assert(frame.width() == reference.width);
        assert(frame.height() == reference.height);
        assert(frame.plane_count() == 3);
        for (int plane_index = 0; plane_index < 3; ++plane_index)
        {
            const ConstPlaneView plane = frame.plane(plane_index);
            const int width = plane_index == 0 ? reference.width : reference.chroma_width;
            const int height = plane_index == 0 ? reference.height : reference.chroma_height;
            assert(plane.width == width);
            assert(plane.height == height);
            for (int row = 0; row < height; ++row)
            {
                const std::uint8_t* expected = reference.planes[static_cast<std::size_t>(plane_index)].data() +
                                               static_cast<std::size_t>(row) * static_cast<std::size_t>(width);
                assert(std::equal(plane.row(row), plane.row(row) + width, expected));
            }
        }
    }

    void test_a_444_grid_assembles_to_the_reference_decode()
    {
        const DecodedImage result = decode_image(Source::from_path(data_path(grid_444_fixture)));
        assert(result.frame.pixel_format() == PixelFormat::from_name("yuv444p"));
        assert_matches_reference(result.frame, read_y4m(grid_444_reference));
    }

    void test_a_420_grid_places_chroma_at_half_the_offset()
    {
        const DecodedImage result = decode_image(Source::from_path(data_path(grid_420_fixture)));
        assert(result.frame.pixel_format() == PixelFormat::from_name("yuv420p"));
        assert_matches_reference(result.frame, read_y4m(grid_420_reference));
    }

    void test_the_record_describes_the_grid()
    {
        const DecodedImage result = decode_image(Source::from_path(data_path(grid_444_fixture)));
        const TileGrid* grid = result.tile_grid();
        assert(grid != nullptr && grid->is_primary);
        assert(result.record.params.at("tile_grid_id") == grid->id);
        assert(grid->tiles.size() == 4);
        assert(result.stream().width == 64 && result.stream().height == 64);
        assert(result.stream().index == grid->tiles.front().stream_index);
        assert(grid->coded_width == 128 && grid->coded_height == 128);
        assert(grid->crop_x == 0 && grid->crop_y == 0);
        assert(grid->width == 128 && grid->height == 96);
        assert(result.record.transform.is_identity());
        assert(result.record.output.width == 128 && result.record.output.height == 96);
    }

    void test_only_the_primary_grid_is_assembled()
    {
        // The file also carries a grid of alpha tiles, which stays out of the
        // decoded image; see ProbeResult::additional_images().
        const DecodedImage result = decode_image(Source::from_path(data_path(grid_alpha_fixture)));
        assert(result.frame.width() == 128 && result.frame.height() == 96);
        assert(result.frame.pixel_format() == PixelFormat::from_name("yuv444p"));
    }

    void test_a_grid_decodes_the_same_from_memory()
    {
        const DecodedImage from_path = decode_image(Source::from_path(data_path(grid_420_fixture)));
        const DecodedImage from_memory = decode_image(Source::from_bytes(read_file(data_path(grid_420_fixture))));
        assert_matches_reference(from_memory.frame, read_y4m(grid_420_reference));
        assert(from_path.tile_grid()->to_json() == from_memory.tile_grid()->to_json());
    }
}

int main()
{
    const Capabilities& caps = capabilities();
    if (!caps.has_decoder("libdav1d") && !caps.has_decoder("libaom-av1"))
    {
        return 77;  // no software AV1 decoder in this build, so AVIF tiles cannot be decoded
    }

    test_a_444_grid_assembles_to_the_reference_decode();
    test_a_420_grid_places_chroma_at_half_the_offset();
    test_the_record_describes_the_grid();
    test_only_the_primary_grid_is_assembled();
    test_a_grid_decodes_the_same_from_memory();
    return 0;
}
