#include "lossylab/convert/convert.hpp"
#include "lossylab/core/error.hpp"
#include "lossylab/io/decode_image.hpp"
#include "lossylab/io/probe.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
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
        const DecodedImage decoded = decode_image(Source::from_bytes(std::vector<std::uint8_t>(file), "webp"), options);
        assert(decoded.stream().orientation == 8);
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

    DecodedImage decode(const std::string_view name, const OrientationHandling handling,
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
        const DecodedImage result = decode("testsrc_64x48_p3_orientation6.jpg", OrientationHandling::Report);
        assert(result.frame.width() == 64 && result.frame.height() == 48);
        assert(result.record.transform.is_identity());
        assert(result.stream().orientation == 6);
        assert(result.stream().orientation_source == "exif");
        assert(result.evidence().orientation_handling == "reported");
    }

    void test_applying_matches_pillows_upright_rendering_for_every_orientation()
    {
        for (int orientation = 2; orientation <= 8; ++orientation)
        {
            const std::string stored = "testsrc_64x48_orientation" + std::to_string(orientation) + ".png";
            const DecodedImage applied = decode(stored, OrientationHandling::Apply);
            const Frame upright =
                decode("testsrc_64x48_orientation" + std::to_string(orientation) + "_upright.png",
                       OrientationHandling::Report)
                    .frame;
            assert(planes_equal(applied.frame, upright));
            assert(applied.evidence().orientation_handling == "applied");
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
        const DecodedImage applied = decode("testsrc_64x48_422_orientation6.jpg", OrientationHandling::Apply);
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
        const DecodedImage single = decode("testsrc_128x96_p3_irot1.avif", OrientationHandling::Apply);
        assert(single.frame.width() == 96 && single.frame.height() == 128);
        assert(single.stream().orientation == 8);

        const DecodedImage grid = decode("testsrc_128x96_grid_irot3.avif", OrientationHandling::Apply);
        assert(grid.frame.width() == 96 && grid.frame.height() == 128);
        assert(grid.tile_grid() != nullptr);
        assert(grid.tile_grid()->orientation == 6);
    }

    void test_a_recognized_profile_stands_in_for_missing_tags()
    {
        const DecodedImage result = decode("testsrc_64x48_p3_orientation6.jpg", OrientationHandling::Report);
        assert(result.frame.color().primaries == ColorPrimaries::Smpte432);
        assert(result.frame.color().transfer == TransferCharacteristic::Srgb);
        assert(result.stream().icc_profile->known_as == "Display P3");

        const auto from_profile = std::find_if(result.record.conversions.begin(), result.record.conversions.end(),
                                               [](const ConversionEvent& event)
                                               { return event.performed_by == "icc_profile"; });
        assert(from_profile != result.record.conversions.end());

        // An AVIF whose only color description is its profile.
        const Frame avif = decode("testsrc_128x96_p3_irot1.avif", OrientationHandling::Report).frame;
        assert(avif.color().primaries == ColorPrimaries::Smpte432);
    }

    DecodeImageOptions adobe_rgb_webp_to_srgb(const IccHandling icc)
    {
        // The WebP decodes to YUV with no chroma siting; sRGB names none.
        DecodeImageOptions options;
        options.assumed_color = ColorSpec::srgb();
        options.assumed_color.chroma_location = ChromaLocation::Center;
        options.conversion = ConvertOptions{};
        options.conversion->pixel_format = PixelFormat::from_name("rgb24");
        options.conversion->color = ColorSpec::srgb();
        options.conversion->icc = icc;
        return options;
    }

    void test_a_profile_without_a_tag_equivalent_is_converted_to_srgb()
    {
        const Source webp = Source::from_path(data_path("testsrc_64x48_lossy_adobe_rgb_orientation8.webp"));
        const DecodedImage converted = decode_image(webp, adobe_rgb_webp_to_srgb(IccHandling::Convert));
        assert(converted.frame.pixel_format().name() == std::string("rgb24"));
        assert(converted.frame.icc_profile() == nullptr);
        assert(converted.record.output.icc_profile.empty());
        assert(has_conversion(converted.record.conversions, "icc_profile"));

        // Adobe RGB's wider gamut moves saturated colors, so the samples
        // differ from the unconverted decode.
        const DecodedImage kept = decode_image(webp, adobe_rgb_webp_to_srgb(IccHandling::Ignore));
        bool differs = false;
        for (int y = 0; y < kept.frame.height() && !differs; ++y)
        {
            differs = std::memcmp(kept.frame.plane(0).row(y), converted.frame.plane(0).row(y),
                                  static_cast<std::size_t>(kept.frame.width() * 3)) != 0;
        }
        assert(differs);
    }

    void test_an_ignored_profile_stays_with_a_converted_frame()
    {
        const Source webp = Source::from_path(data_path("testsrc_64x48_lossy_adobe_rgb_orientation8.webp"));
        const DecodedImage converted = decode_image(webp, adobe_rgb_webp_to_srgb(IccHandling::Ignore));
        assert(converted.frame.icc_profile() != nullptr);
        assert(converted.frame.icc_profile()->info.known_as == "Adobe RGB (1998)");
        assert(converted.record.output.icc_profile == "Adobe RGB (1998)");
        assert(!has_conversion(converted.record.conversions, "icc_profile"));
    }

    void test_relabeling_the_primaries_drops_the_profile()
    {
        const Frame frame = decode("testsrc_64x48_p3_orientation6.jpg", OrientationHandling::Report).frame;
        ColorSpec relabeled = frame.color();
        relabeled.primaries = ColorPrimaries::Bt709;
        const FrameResult result = reinterpret(frame, relabeled);
        assert(result.frame.icc_profile() == nullptr);
        assert(result.record.input.icc_profile == "Display P3");
        assert(result.record.output.icc_profile.empty());
        const auto dropped = std::find_if(result.record.conversions.begin(), result.record.conversions.end(),
                                          [](const ConversionEvent& event)
                                          { return event.property == "icc_profile"; });
        assert(dropped != result.record.conversions.end());
        assert(dropped->from == "Display P3" && dropped->to == "dropped");

        // Relabeling only the range leaves the profile's primaries and tone curve.
        ColorSpec full_range = frame.color();
        full_range.range = ColorRange::Limited;
        assert(reinterpret(frame, full_range).frame.icc_profile() != nullptr);
    }

    void test_the_decoded_frame_carries_the_embedded_data()
    {
        const DecodedImage reported = decode("testsrc_64x48_p3_orientation6.jpg", OrientationHandling::Report);
        assert(reported.frame.icc_profile()->info.known_as == "Display P3");
        assert(reported.frame.orientation() == 6);
        assert(reported.frame.sample_aspect_ratio() == (Rational{1, 1}));
        assert(reported.record.output.orientation == 6);
        assert(reported.record.output.icc_profile == "Display P3");

        // Applied, the orientation is gone from the frame; the profile stays.
        const DecodedImage applied = decode("testsrc_64x48_p3_orientation6.jpg", OrientationHandling::Apply);
        assert(!applied.frame.orientation().has_value());
        assert(applied.record.input.orientation == 6);
        assert(!applied.record.output.orientation.has_value());
        assert(applied.frame.icc_profile()->info.known_as == "Display P3");

        // An AVIF carries its profile and its irot as an orientation; a grid
        // image carries the grid's.
        const DecodedImage single = decode("testsrc_128x96_p3_irot1.avif", OrientationHandling::Report);
        assert(single.frame.icc_profile()->info.known_as == "Display P3");
        assert(single.frame.orientation() == 8);
        const DecodedImage grid = decode("testsrc_128x96_grid_irot3.avif", OrientationHandling::Report);
        assert(grid.frame.orientation() == 6);
        assert((grid.frame.icc_profile() != nullptr) == grid.tile_grid()->icc_profile.has_value());

        // A frame without embedded data states that too.
        const Frame plain = decode("testsrc_64x48.png", OrientationHandling::Report).frame;
        assert(plain.icc_profile() == nullptr && !plain.orientation().has_value());
    }

    void test_a_file_without_embedded_data_records_its_absence()
    {
        const DecodedImage result = decode("testsrc_64x48.png", OrientationHandling::Apply);
        assert(!result.stream().icc_profile.has_value());
        assert(result.stream().icc_profile_availability == Availability::NotPresent);
        assert(!result.stream().orientation.has_value());
        assert(result.stream().orientation_availability == Availability::NotPresent);
        assert(result.evidence().orientation_handling == "reported");
        assert(result.record.transform.is_identity());
    }

    void test_the_decode_input_is_the_color_the_file_tags()
    {
        // The JPEG tags matrix and range only; the rest is assumed.
        const DecodedImage assumed = decode("testsrc_64x48.jpg", OrientationHandling::Report);
        assert(assumed.record.input.color == assumed.stream().color);
        assert(assumed.record.input.color.primaries == ColorPrimaries::Unspecified);
        assert(assumed.record.output.color.primaries == ColorPrimaries::Bt709);
        assert(assumed.record.conversions.front().from == assumed.record.input.color.describe());

        // The profile names the primaries, but the file's tags do not.
        const DecodedImage profiled = decode("testsrc_64x48_p3_orientation6.jpg", OrientationHandling::Report);
        assert(profiled.record.input.color.primaries == ColorPrimaries::Unspecified);
        assert(profiled.record.output.color.primaries == ColorPrimaries::Smpte432);
    }

    void test_decoding_completes_what_probe_cannot_read()
    {
        const StreamInfo& probed = probe_stream("testsrc_64x48.tif");
        assert(probed.orientation_availability == Availability::NotSupportedByBuild);
        assert(probed.icc_profile_availability == Availability::NotSupportedByBuild);

        const DecodedImage decoded = decode("testsrc_64x48.tif", OrientationHandling::Report);
        assert(decoded.stream().orientation_availability == Availability::NotPresent);
        assert(decoded.stream().icc_profile_availability == Availability::NotPresent);
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
    test_a_profile_without_a_tag_equivalent_is_converted_to_srgb();
    test_an_ignored_profile_stays_with_a_converted_frame();
    test_relabeling_the_primaries_drops_the_profile();
    test_the_decoded_frame_carries_the_embedded_data();
    test_a_file_without_embedded_data_records_its_absence();
    test_the_decode_input_is_the_color_the_file_tags();
    test_decoding_completes_what_probe_cannot_read();
    std::puts("test_icc_orientation: all passed");
    return 0;
}
