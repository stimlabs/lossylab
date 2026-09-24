#include "lossylab/core/error.hpp"
#include "lossylab/io/decode_image.hpp"
#include "lossylab/io/probe.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

using namespace lossylab;

namespace
{
    std::string data_path(const std::string_view name)
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

    const StreamInfo& probe_stream(const std::string_view name)
    {
        static ProbeResult result;
        result = probe(Source::from_path(data_path(name)));
        assert(result.streams.size() >= 1);
        return result.streams.front();
    }

    // ---- probe --------------------------------------------------------------

    void test_a_jpeg_reports_its_exif_orientation_and_icc_profile()
    {
        const StreamInfo& stream = probe_stream("testsrc_64x48_p3_orientation6.jpg");
        assert(stream.orientation == 6);
        assert(stream.orientation_source == "exif");
        assert(stream.orientation_availability == Availability::Present);

        assert(stream.icc_profile_availability == Availability::Present);
        assert(stream.icc_profile.has_value());
        assert(stream.icc_profile->known_as == "Display P3");
        assert(stream.icc_profile->problems.empty());

        // A JPEG tags no primaries or transfer, so there is nothing to disagree with.
        assert(!stream.icc_matches_tagged_color.has_value());
    }

    void test_a_profile_split_across_app2_segments_is_reassembled()
    {
        const StreamInfo& stream = probe_stream("testsrc_64x48_p3_sampled.jpg");
        const auto icc_segments = std::count_if(stream.jpeg->segments.begin(), stream.jpeg->segments.end(),
                                                [](const JpegInfo::Segment& segment)
                                                { return segment.identifier == "ICC_PROFILE"; });
        assert(icc_segments == 4);
        assert(stream.icc_profile->size_bytes == 197160);
        assert(stream.icc_profile->problems.empty());
        assert(stream.icc_profile->description == "Display P3 sampled");
        assert(stream.icc_profile->known_as == "Display P3");
    }

    void test_a_missing_app2_chunk_is_a_problem()
    {
        // Removes the second ICC_PROFILE segment, marker and payload.
        std::vector<std::uint8_t> bytes = read_file(data_path("testsrc_64x48_p3_sampled.jpg"));
        const std::array<std::uint8_t, 2> app2 = {0xff, 0xe2};
        auto segment = std::search(bytes.begin(), bytes.end(), app2.begin(), app2.end());
        segment = std::search(segment + 2, bytes.end(), app2.begin(), app2.end());
        assert(segment != bytes.end());
        const std::ptrdiff_t length = (segment[2] << 8) | segment[3];
        bytes.erase(segment, segment + 2 + length);

        const ProbeResult result = probe(Source::from_bytes(std::move(bytes)));
        const IccProfileInfo& profile = *result.streams.front().icc_profile;
        assert(profile.problems.front() == "ICC_PROFILE chunk 2 of 4 is missing");
    }

    void test_a_file_without_embedded_data_reports_their_absence()
    {
        const StreamInfo& stream = probe_stream("testsrc_64x48.jpg");
        assert(!stream.orientation.has_value());
        assert(stream.orientation_availability == Availability::NotPresent);
        assert(!stream.icc_profile.has_value());
        assert(stream.icc_profile_availability == Availability::NotPresent);
    }

    void test_a_webp_reports_its_chunks()
    {
        const StreamInfo& stream = probe_stream("testsrc_64x48_lossy_adobe_rgb_orientation8.webp");
        assert(stream.orientation == 8);
        assert(stream.orientation_source == "exif");
        assert(stream.icc_profile->known_as == "Adobe RGB (1998)");
        assert(stream.image_container->compression == std::optional<std::string>("lossy"));
    }

    /// The Adobe RGB, orientation 8 WebP with its RIFF size rewritten after
    /// `edit` changes the chunk bytes that follow the 12-byte RIFF header.
    std::vector<std::uint8_t> oriented_webp_with_chunks(
        const std::function<std::vector<std::uint8_t>(const std::vector<std::uint8_t>&)>& edit)
    {
        const std::vector<std::uint8_t> original =
            read_file(data_path("testsrc_64x48_lossy_adobe_rgb_orientation8.webp"));
        const std::vector<std::uint8_t> chunks = edit({original.begin() + 12, original.end()});
        std::vector<std::uint8_t> file = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'E', 'B', 'P'};
        file.insert(file.end(), chunks.begin(), chunks.end());
        const auto riff_size = static_cast<std::uint32_t>(file.size() - 8);
        for (int byte = 0; byte < 4; ++byte)
        {
            file[4 + byte] = static_cast<std::uint8_t>(riff_size >> (8 * byte));
        }
        return file;
    }

    void test_a_webp_exif_chunk_with_the_jpeg_prefix_still_gives_its_orientation()
    {
        // FFmpeg's WebP decoder rejects the "Exif\0\0" prefix. The fixture's
        // EXIF chunk is its last: an 8-byte header and 26 bytes of payload.
        const std::vector<std::uint8_t> file = oriented_webp_with_chunks(
            [](const std::vector<std::uint8_t>& chunks)
            {
                constexpr std::size_t exif_payload_size = 26;
                std::vector<std::uint8_t> edited(chunks.begin(), chunks.end() - (8 + exif_payload_size));
                const std::array<std::uint8_t, 14> header_and_prefix = {'E', 'X', 'I', 'F', 32, 0, 0, 0,
                                                                        'E', 'x', 'i', 'f', 0,  0};
                edited.insert(edited.end(), header_and_prefix.begin(), header_and_prefix.end());
                edited.insert(edited.end(), chunks.end() - exif_payload_size, chunks.end());
                return edited;
            });

        const ProbeResult probed = probe(Source::from_bytes(std::vector<std::uint8_t>(file), "webp"));
        assert(probed.streams.front().orientation == 8);

        DecodeImageOptions options;
        options.orientation = OrientationHandling::Report;
        const FrameResult decoded = decode_image(Source::from_bytes(std::vector<std::uint8_t>(file), "webp"), options);
        assert(decoded.record.params.at("orientation") == 8);
    }

    void test_a_webp_xmp_chunk_is_reported_as_present()
    {
        const StreamInfo& without = probe_stream("testsrc_64x48_lossy_adobe_rgb_orientation8.webp");
        assert(without.image_container->has_xmp == std::optional<bool>(false));

        const std::string xmp = "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\"/>";
        const std::vector<std::uint8_t> file = oriented_webp_with_chunks(
            [&xmp](std::vector<std::uint8_t> chunks)
            {
                chunks.insert(chunks.end(), {'X', 'M', 'P', ' ', static_cast<std::uint8_t>(xmp.size()), 0, 0, 0});
                chunks.insert(chunks.end(), xmp.begin(), xmp.end());
                if (xmp.size() % 2 == 1)
                {
                    chunks.push_back(0);
                }
                return chunks;
            });
        const ProbeResult probed = probe(Source::from_bytes(std::vector<std::uint8_t>(file), "webp"));
        assert(probed.streams.front().image_container->has_xmp == std::optional<bool>(true));
    }

    void test_a_png_reports_its_exif_and_its_compressed_profile()
    {
        const StreamInfo& oriented = probe_stream("testsrc_64x48_orientation6.png");
        assert(oriented.orientation == 6);
        assert(oriented.icc_profile_availability == Availability::NotPresent);

        const StreamInfo& with_profile = probe_stream("testsrc_64x48_p3.png");
        assert(with_profile.icc_profile->known_as == "Display P3");
        assert(with_profile.orientation_availability == Availability::NotPresent);
    }

    void test_every_png_orientation_is_read()
    {
        for (int orientation = 2; orientation <= 8; ++orientation)
        {
            const StreamInfo& stream =
                probe_stream("testsrc_64x48_orientation" + std::to_string(orientation) + ".png");
            assert(stream.orientation == orientation);
        }
    }

    void test_an_avif_reports_its_rotation_property_as_an_orientation()
    {
        // irot 1 is a quarter turn anticlockwise: EXIF orientation 8.
        const StreamInfo& stream = probe_stream("testsrc_128x96_p3_irot1.avif");
        assert(stream.orientation == 8);
        assert(stream.orientation_source == "irot_imir");
        assert(stream.rotation.has_value());
        assert(stream.icc_profile->known_as == "Display P3");
    }

    void test_an_avif_mirror_property_is_an_orientation()
    {
        // imir 0 mirrors top to bottom: EXIF orientation 4, which a rotation
        // angle alone cannot express.
        const StreamInfo& stream = probe_stream("testsrc_128x96_imir0.avif");
        assert(stream.orientation == 4);
        assert(!stream.icc_profile.has_value());
    }

    void test_a_video_has_no_orientation_or_profile()
    {
        const StreamInfo& stream = probe_stream("testsrc_64x48.mp4");
        assert(stream.orientation_availability == Availability::NotPresent);
        assert(stream.icc_profile_availability == Availability::NotPresent);
    }

    void test_the_new_fields_serialize()
    {
        const json::Value value = probe(Source::from_path(data_path("testsrc_64x48_p3_orientation6.jpg"))).to_json();
        const json::Value& stream = value.at("streams").at(0);
        assert(stream.at("orientation") == 6);
        assert(stream.at("orientation_source").get<std::string>() == "exif");
        assert(stream.at("orientation_availability").get<std::string>() == "present");
        assert(stream.at("icc_profile").at("known_as").get<std::string>() == "Display P3");
        assert(stream.at("icc_profile_availability").get<std::string>() == "present");
        assert(stream.at("icc_matches_tagged_color").is_null());

        const json::Value grid =
            probe(Source::from_path(data_path("testsrc_128x96_grid_irot3.avif"))).to_json().at("tile_grids").at(0);
        assert(grid.at("orientation") == 6);
    }

    // ---- decode_image -------------------------------------------------------

    FrameResult decode(const std::string_view name, const OrientationHandling handling,
                       const Strict strict = Strict::AllowRecorded)
    {
        DecodeImageOptions options;
        options.orientation = handling;
        options.strict = strict;
        return decode_image(Source::from_path(data_path(name)), options);
    }

    bool planes_equal(const Frame& left, const Frame& right)
    {
        if (left.pixel_format() != right.pixel_format() || left.width() != right.width() ||
            left.height() != right.height())
        {
            return false;
        }
        for (int plane_index = 0; plane_index < left.plane_count(); ++plane_index)
        {
            const ConstPlaneView left_plane = left.plane(plane_index);
            const ConstPlaneView right_plane = right.plane(plane_index);
            for (int row = 0; row < left_plane.height; ++row)
            {
                if (!std::equal(left_plane.row(row), left_plane.row(row) + left_plane.row_bytes(),
                                right_plane.row(row)))
                {
                    return false;
                }
            }
        }
        return true;
    }

    bool has_conversion(const ConversionList& conversions, const std::string& property)
    {
        return std::any_of(conversions.begin(), conversions.end(),
                           [&property](const ConversionEvent& event) { return event.property == property; });
    }

    void test_by_default_the_orientation_is_reported_not_applied()
    {
        const FrameResult result = decode("testsrc_64x48_p3_orientation6.jpg", OrientationHandling::Report);
        assert(result.frame.width() == 64 && result.frame.height() == 48);
        assert(result.record.transform.is_identity());
        assert(result.record.params.at("orientation") == 6);
        assert(result.record.params.at("orientation_handling").get<std::string>() == "reported");
    }

    void test_applying_matches_pillows_upright_rendering_for_every_orientation()
    {
        for (int orientation = 2; orientation <= 8; ++orientation)
        {
            const std::string stored = "testsrc_64x48_orientation" + std::to_string(orientation) + ".png";
            const FrameResult applied = decode(stored, OrientationHandling::Apply);
            const Frame upright =
                decode("testsrc_64x48_orientation" + std::to_string(orientation) + "_upright.png",
                       OrientationHandling::Report)
                    .frame;
            assert(planes_equal(applied.frame, upright));
            assert(applied.record.params.at("orientation_handling").get<std::string>() == "applied");
            assert(applied.record.transform == CoordinateTransform::orientation(orientation, 64, 48));
            assert(applied.record.output.width == upright.width());
        }
    }

    void test_applying_a_quarter_turn_moves_every_plane_exactly()
    {
        const Frame stored = decode("testsrc_64x48_p3_orientation6.jpg", OrientationHandling::Report).frame;
        const Frame applied = decode("testsrc_64x48_p3_orientation6.jpg", OrientationHandling::Apply).frame;
        assert(applied.pixel_format() == stored.pixel_format());
        assert(applied.width() == 48 && applied.height() == 64);

        // Clockwise: stored (x, y) shows at (height - 1 - y, x), on every
        // plane at that plane's own size, since 4:2:0 transposes to itself.
        for (int plane_index = 0; plane_index < stored.plane_count(); ++plane_index)
        {
            const ConstPlaneView from = stored.plane(plane_index);
            const ConstPlaneView to = applied.plane(plane_index);
            assert(to.width == from.height && to.height == from.width);
            for (int y = 0; y < from.height; ++y)
            {
                for (int x = 0; x < from.width; ++x)
                {
                    assert(to.row(x)[from.height - 1 - y] == from.row(y)[x]);
                }
            }
        }
    }

    void test_a_transposed_422_image_becomes_440_and_says_so()
    {
        const FrameResult applied = decode("testsrc_64x48_422_orientation6.jpg", OrientationHandling::Apply);
        assert(applied.frame.pixel_format().subsampling() == Subsampling::Yuv440);
        assert(has_conversion(applied.record.conversions, "subsampling"));

        bool refused = false;
        try
        {
            (void)decode("testsrc_64x48_422_orientation6.jpg", OrientationHandling::Apply, Strict::Refuse);
        }
        catch (const ConversionRefused&)
        {
            refused = true;
        }
        assert(refused);

        // Reporting changes nothing, so there is nothing to refuse.
        (void)decode("testsrc_64x48_422_orientation6.jpg", OrientationHandling::Report, Strict::Refuse);
    }

    void test_an_avif_and_a_grid_are_turned_upright()
    {
        const FrameResult single = decode("testsrc_128x96_p3_irot1.avif", OrientationHandling::Apply);
        assert(single.frame.width() == 96 && single.frame.height() == 128);
        assert(single.record.params.at("orientation") == 8);

        const FrameResult grid = decode("testsrc_128x96_grid_irot3.avif", OrientationHandling::Apply);
        assert(grid.frame.width() == 96 && grid.frame.height() == 128);
        assert(grid.record.params.at("orientation") == 6);
        assert(!grid.record.params.at("tile_grid").is_null());
    }

    void test_a_recognized_profile_stands_in_for_missing_tags()
    {
        const FrameResult result = decode("testsrc_64x48_p3_orientation6.jpg", OrientationHandling::Report);
        assert(result.frame.color().primaries == ColorPrimaries::Smpte432);
        assert(result.frame.color().transfer == TransferCharacteristic::Srgb);
        assert(result.record.params.at("icc_profile").at("known_as").get<std::string>() == "Display P3");

        const auto from_profile = std::find_if(result.record.conversions.begin(), result.record.conversions.end(),
                                               [](const ConversionEvent& event)
                                               { return event.performed_by == "icc_profile"; });
        assert(from_profile != result.record.conversions.end());

        // An AVIF whose only color description is its profile.
        const Frame avif = decode("testsrc_128x96_p3_irot1.avif", OrientationHandling::Report).frame;
        assert(avif.color().primaries == ColorPrimaries::Smpte432);
    }

    void test_a_profile_without_a_tag_equivalent_blocks_a_color_conversion_under_refuse()
    {
        // The WebP decodes to YUV with no chroma siting; sRGB names none.
        DecodeImageOptions options;
        options.assumed_color = ColorSpec::srgb();
        options.assumed_color.chroma_location = ChromaLocation::Center;
        options.pixel_format = PixelFormat::from_name("rgb24");
        options.color = ColorSpec::srgb();
        options.strict = Strict::Refuse;
        const Source webp = Source::from_path(data_path("testsrc_64x48_lossy_adobe_rgb_orientation8.webp"));

        bool refused = false;
        try
        {
            (void)decode_image(webp, options);
        }
        catch (const ConversionRefused&)
        {
            refused = true;
        }
        assert(refused);

        options.strict = Strict::AllowRecorded;
        const FrameResult allowed = decode_image(webp, options);
        const auto ignored = std::find_if(allowed.record.conversions.begin(), allowed.record.conversions.end(),
                                          [](const ConversionEvent& event)
                                          { return event.property == "icc_profile"; });
        assert(ignored != allowed.record.conversions.end());
        assert(ignored->from == "Adobe RGB (1998)" && ignored->to == "ignored");

        // Without a conversion there is nothing the profile could be ignored by.
        const FrameResult reported = decode("testsrc_64x48_lossy_adobe_rgb_orientation8.webp",
                                            OrientationHandling::Report, Strict::Refuse);
        assert(!has_conversion(reported.record.conversions, "icc_profile"));
        assert(reported.frame.color().primaries == ColorPrimaries::Bt709);
    }

    void test_a_file_without_embedded_data_records_its_absence()
    {
        const FrameResult result = decode("testsrc_64x48.png", OrientationHandling::Apply);
        assert(result.record.params.at("icc_profile").is_null());
        assert(result.record.params.at("orientation").is_null());
        assert(result.record.params.at("orientation_handling").get<std::string>() == "reported");
        assert(result.record.transform.is_identity());
    }
}

int main()
{
    test_a_jpeg_reports_its_exif_orientation_and_icc_profile();
    test_a_profile_split_across_app2_segments_is_reassembled();
    test_a_missing_app2_chunk_is_a_problem();
    test_a_file_without_embedded_data_reports_their_absence();
    test_a_webp_reports_its_chunks();
    test_a_webp_exif_chunk_with_the_jpeg_prefix_still_gives_its_orientation();
    test_a_webp_xmp_chunk_is_reported_as_present();
    test_a_png_reports_its_exif_and_its_compressed_profile();
    test_every_png_orientation_is_read();
    test_an_avif_reports_its_rotation_property_as_an_orientation();
    test_an_avif_mirror_property_is_an_orientation();
    test_a_video_has_no_orientation_or_profile();
    test_the_new_fields_serialize();
    test_by_default_the_orientation_is_reported_not_applied();
    test_applying_matches_pillows_upright_rendering_for_every_orientation();
    test_applying_a_quarter_turn_moves_every_plane_exactly();
    test_a_transposed_422_image_becomes_440_and_says_so();
    test_an_avif_and_a_grid_are_turned_upright();
    test_a_recognized_profile_stands_in_for_missing_tags();
    test_a_profile_without_a_tag_equivalent_blocks_a_color_conversion_under_refuse();
    test_a_file_without_embedded_data_records_its_absence();
    std::puts("test_icc_orientation: all passed");
    return 0;
}
