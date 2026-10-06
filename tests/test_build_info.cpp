#include "lossylab/env/build_info.hpp"

#include <cassert>

using namespace lossylab;

namespace
{
    void test_build_info_reports_a_version_and_libraries()
    {
        const FfmpegBuild& info = build_info().ffmpeg;

        assert(!info.version.empty());
        assert(info.configure_hash.starts_with("sha256:"));
        assert(info.libraries.size() == std::size_t{6});

        for (const LibraryVersion& library : info.libraries)
        {
            assert(!library.name.empty());
            assert(library.major > 0);
        }
    }

    void test_runtime_libraries_match_the_headers_we_compiled_against()
    {
        // A mismatch means the library was built against one FFmpeg and is running
        // against another. Everything downstream would still appear to work while
        // producing subtly different output, so it is worth an explicit check.
        const FfmpegBuild& info = build_info().ffmpeg;
        for (const LibraryVersion& library : info.libraries)
        {
            assert(library.matches_compiled());
        }
        assert(info.is_consistent());
    }

    void test_the_libraries_beside_ffmpeg_are_part_of_the_identity()
    {
        const LossylabBuild& info = build_info().lossylab;
        assert(info.libraries.size() == std::size_t{2});
        assert(info.libraries[0].name == "lcms2");
        assert(info.libraries[1].name == "zlib");
        for (const LibraryVersion& library : info.libraries)
        {
            assert(library.major > 0);
            assert(library.major == library.compiled_major);
        }

        json::Value other = build_info().to_json();
        other["lossylab"]["libraries"]["lcms2"] = "0.0.0";
        assert(build_diff(build_info().to_json(), other).contains("lossylab.libraries.lcms2"));
    }

    void test_the_identity_names_the_commit_and_hashes_everything()
    {
        const BuildInfo& info = build_info();

        // From git at build time: a full SHA-1 commit, never a placeholder.
        assert(info.lossylab.commit.size() == std::size_t{40});
        assert(!info.lossylab.compiler.empty());

        // "sha256:" and 64 hex characters. Every record carries this, so two
        // calls must agree.
        assert(info.identity_hash.size() == std::string_view("sha256:").size() + 64);
        assert(info.identity_hash.starts_with("sha256:"));
        assert(build_info().identity_hash == info.identity_hash);
    }

    void test_build_diff_names_what_differs()
    {
        const json::Value a = build_info().to_json();
        assert(build_diff(a, a).empty());

        json::Value other = a;
        other["lossylab"]["commit"] = "0000";
        other["ffmpeg"]["libraries"]["libavcodec"] = "0.0.0";
        other["ffmpeg"]["extra"] = 1;

        const json::Value differences = build_diff(a, other);
        assert(differences.size() == std::size_t{3});
        assert(differences.at("lossylab.commit").at(1) == "0000");
        assert(differences.at("ffmpeg.libraries.libavcodec").at(1) == "0.0.0");
        assert(differences.at("ffmpeg.extra").at(0).is_null());
    }

    void test_diagnostics_describe_the_machine()
    {
        const Diagnostics& machine = diagnostics();
        assert(!machine.architecture.empty());
        assert(!machine.os.empty());
        assert(machine.to_json().at("architecture") == machine.architecture);
    }

    void test_the_license_is_identified()
    {
        const FfmpegBuild& info = build_info().ffmpeg;

        // Whatever this build is, it must not come back as Unknown: the license
        // decides whether the library can be redistributed, and guessing is worse
        // than failing.
        assert(info.license != License::Unknown);

        // The restrictive reading is the safe one, so GPL and nonfree must never
        // report as distributable.
        if (info.license == License::Gpl2 || info.license == License::Gpl3 ||
            info.license == License::Nonfree)
        {
            assert(!permits_proprietary_distribution(info.license));
        }
    }

    void test_build_info_serializes_compactly()
    {
        const json::Value document = build_info().to_json();

        assert(document.at("identity_hash").get<std::string>() == build_info().identity_hash);
        assert(!document.at("ffmpeg").at("license").get<std::string>().empty());

        // Library versions are one string each, and the configure line is only
        // a hash.
        const json::Value& libraries = document.at("ffmpeg").at("libraries");
        assert(libraries.size() == 6);
        assert(libraries.at("libavcodec").is_string());
        assert(document.at("ffmpeg").at("configure_hash").get<std::string>() == build_info().ffmpeg.configure_hash);
        assert(!document.at("ffmpeg").contains("configuration"));
        assert(document.at("lossylab").at("commit").get<std::string>() == build_info().lossylab.commit);

        // Round-trips through the library's own serializer.
        assert(json::parse(document.dump()) == document);
    }
}

int main()
{
    test_build_info_reports_a_version_and_libraries();
    test_runtime_libraries_match_the_headers_we_compiled_against();
    test_the_libraries_beside_ffmpeg_are_part_of_the_identity();
    test_the_identity_names_the_commit_and_hashes_everything();
    test_build_diff_names_what_differs();
    test_diagnostics_describe_the_machine();
    test_the_license_is_identified();
    test_build_info_serializes_compactly();
}
