#include "lossylab/io/icc_profile.hpp"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace lossylab;

namespace
{
    std::vector<std::uint8_t> read_profile(const std::string& name)
    {
        const std::string path = std::string(LOSSYLAB_TEST_DATA_DIR) + "/icc/" + name;
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

    bool near(const Chromaticity& measured, const double x, const double y)
    {
        return std::abs(measured.x - x) < 0.002 && std::abs(measured.y - y) < 0.002;
    }

    void test_a_display_p3_profile_is_recognized_from_its_tags()
    {
        const IccProfileInfo info = describe_icc_profile(read_profile("display_p3_v4.icc"));

        assert(info.problems.empty());
        assert(info.version == "4.3");
        assert(info.device_class == "display");
        assert(info.data_color_space == "RGB");
        assert(info.connection_space == "XYZ");
        assert(info.creator == "llab");
        assert(info.description == "Display P3");
        assert(info.copyright == std::optional<std::string>("No copyright, use freely"));
        assert(info.is_matrix_shaper);
        assert(!info.has_lookup_table);

        assert(info.colorants.has_value());
        assert(near(info.colorants->red, 0.680, 0.320));
        assert(near(info.colorants->green, 0.265, 0.690));
        assert(near(info.colorants->blue, 0.150, 0.060));
        assert(near(info.colorants->white, 0.3127, 0.3290));

        assert(info.transfer_curve == "srgb");
        assert(info.primaries == ColorPrimaries::Smpte432);
        assert(info.transfer == TransferCharacteristic::Srgb);
        assert(info.known_as == "Display P3");
        assert(info.is_expressible_as_tags());
    }

    void test_an_embedded_profile_id_is_reported_as_written()
    {
        const IccProfileInfo info = describe_icc_profile(read_profile("display_p3_v4.icc"));
        assert(info.profile_id_embedded);
        assert(info.profile_id.size() == 32);
    }

    void test_a_sampled_tone_curve_is_matched_to_srgb()
    {
        const IccProfileInfo info = describe_icc_profile(read_profile("display_p3_sampled_v4.icc"));
        assert(info.problems.empty());
        assert(info.transfer_curve == "srgb");
        assert(info.known_as == "Display P3");
    }

    void test_a_version_2_srgb_profile_is_recognized()
    {
        const IccProfileInfo info = describe_icc_profile(read_profile("srgb_colord.icc"));
        assert(info.problems.empty());
        assert(info.primaries == ColorPrimaries::Bt709);
        assert(info.transfer == TransferCharacteristic::Srgb);
        assert(info.known_as == "sRGB");
    }

    void test_a_rec709_profile_has_the_bt709_curve()
    {
        const IccProfileInfo info = describe_icc_profile(read_profile("rec709_colord.icc"));
        assert(info.primaries == ColorPrimaries::Bt709);
        assert(info.transfer_curve == "bt709");
        assert(info.transfer == TransferCharacteristic::Bt709);
        assert(info.known_as == "BT.709");
    }

    void test_adobe_rgb_is_named_but_has_no_primaries_code()
    {
        const IccProfileInfo info = describe_icc_profile(read_profile("adobe_rgb_colord.icc"));
        assert(info.known_as == "Adobe RGB (1998)");
        assert(!info.primaries.has_value());
        assert(info.transfer == TransferCharacteristic::Gamma22);
        assert(!info.is_expressible_as_tags());
        assert(near(info.colorants->green, 0.210, 0.710));
    }

    void test_prophoto_keeps_its_d50_white_and_gamma()
    {
        const IccProfileInfo info = describe_icc_profile(read_profile("prophoto_rgb_colord.icc"));
        assert(info.known_as == "ProPhoto RGB");
        assert(info.transfer_curve == "gamma");
        assert(info.gamma.has_value() && std::abs(*info.gamma - 1.8) < 0.01);
        assert(!info.transfer.has_value());
        assert(near(info.colorants->white, 0.3457, 0.3585));
    }

    void test_a_cmyk_lookup_table_profile_describes_no_colorants()
    {
        const IccProfileInfo info = describe_icc_profile(read_profile("cmyk_lut_v4.icc"));
        assert(info.problems.empty());
        assert(info.device_class == "output");
        assert(info.data_color_space == "CMYK");
        assert(info.connection_space == "Lab");
        assert(info.description == "Synthetic CMYK");
        assert(info.has_lookup_table);
        assert(!info.is_matrix_shaper);
        assert(!info.colorants.has_value());
        assert(!info.is_expressible_as_tags());
    }

    void test_a_version_2_gray_profile_reads_its_gamma()
    {
        const IccProfileInfo info = describe_icc_profile(read_profile("gray_gamma22_v2.icc"));
        assert(info.problems.empty());
        assert(info.version == "2.1");
        assert(info.description == "Gray Gamma 2.2");
        assert(info.data_color_space == "GRAY");
        assert(info.is_matrix_shaper);
        assert(info.transfer == TransferCharacteristic::Gamma22);
        assert(!info.primaries.has_value());

        // Version 2 leaves the ID zero, so it is computed.
        assert(!info.profile_id_embedded);
        assert(info.profile_id.size() == 32);
    }

    void test_a_computed_profile_id_matches_the_embedded_one()
    {
        // The fixture's embedded ID was computed the way the specification
        // says; zeroing it makes the parser compute the same value.
        std::vector<std::uint8_t> bytes = read_profile("display_p3_v4.icc");
        const std::string embedded = describe_icc_profile(bytes).profile_id;
        std::fill(bytes.begin() + 84, bytes.begin() + 100, 0);
        const IccProfileInfo computed = describe_icc_profile(bytes);
        assert(!computed.profile_id_embedded);
        assert(computed.profile_id == embedded);
    }

    void test_garbage_is_reported_not_thrown()
    {
        const std::vector<std::uint8_t> short_input(40, 0x41);
        assert(describe_icc_profile(short_input).problems.size() == 1);

        const std::vector<std::uint8_t> not_a_profile(300, 0x41);
        assert(describe_icc_profile(not_a_profile).problems.size() == 1);
        assert(describe_icc_profile({}).problems.size() == 1);
    }

    void test_a_truncated_profile_reports_what_it_could_read()
    {
        std::vector<std::uint8_t> bytes = read_profile("display_p3_v4.icc");
        bytes.resize(300);
        const IccProfileInfo info = describe_icc_profile(bytes);
        assert(!info.problems.empty());
        assert(info.problems.front().starts_with("truncated"));
        assert(info.data_color_space == "RGB");
    }

    void test_every_prefix_of_a_profile_parses_without_crashing()
    {
        const std::vector<std::uint8_t> bytes = read_profile("srgb_colord.icc");
        for (std::size_t length = 0; length <= bytes.size(); length += 7)
        {
            const std::vector<std::uint8_t> prefix(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(length));
            (void)describe_icc_profile(prefix);
        }
    }

    void test_corrupted_tag_offsets_are_problems()
    {
        std::vector<std::uint8_t> bytes = read_profile("display_p3_v4.icc");
        // The first tag entry's offset, pointed far past the end.
        bytes[132 + 4] = 0x7f;
        const IccProfileInfo info = describe_icc_profile(bytes);
        assert(!info.problems.empty());
        assert(info.problems.front().find("outside the profile") != std::string::npos);
    }

    void test_the_description_serializes()
    {
        const json::Value value = describe_icc_profile(read_profile("display_p3_v4.icc")).to_json();
        assert(value.at("known_as").get<std::string>() == "Display P3");
        assert(value.at("primaries").get<std::string>() == "smpte432");
        assert(value.at("colorants").at("red").at("x").get<double>() > 0.67);
    }
}

int main()
{
    test_a_display_p3_profile_is_recognized_from_its_tags();
    test_an_embedded_profile_id_is_reported_as_written();
    test_a_sampled_tone_curve_is_matched_to_srgb();
    test_a_version_2_srgb_profile_is_recognized();
    test_a_rec709_profile_has_the_bt709_curve();
    test_adobe_rgb_is_named_but_has_no_primaries_code();
    test_prophoto_keeps_its_d50_white_and_gamma();
    test_a_cmyk_lookup_table_profile_describes_no_colorants();
    test_a_version_2_gray_profile_reads_its_gamma();
    test_a_computed_profile_id_matches_the_embedded_one();
    test_garbage_is_reported_not_thrown();
    test_a_truncated_profile_reports_what_it_could_read();
    test_every_prefix_of_a_profile_parses_without_crashing();
    test_corrupted_tag_offsets_are_problems();
    test_the_description_serializes();
    std::puts("test_icc_profile: all passed");
    return 0;
}
