#include "lossylab/env/build_info.hpp"

#include <cassert>

using namespace lossylab;

namespace
{
    void test_build_info_reports_a_version_and_libraries()
    {
        const BuildInfo& info = build_info();

        assert(!info.version.empty());
        assert(!info.configuration.empty());
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
        const BuildInfo& info = build_info();
        for (const LibraryVersion& library : info.libraries)
        {
            assert(library.matches_compiled());
        }
        assert(info.is_consistent());
    }

    void test_the_build_id_is_stable_and_non_empty()
    {
        const BuildInfo& info = build_info();
        assert(info.build_id.size() == std::size_t{16});

        // Every record carries this, so two calls must agree.
        assert(build_info().build_id == info.build_id);
    }

    void test_the_license_is_identified()
    {
        const BuildInfo& info = build_info();

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

    void test_external_libraries_are_parsed_from_the_configure_line()
    {
        const BuildInfo& info = build_info();

        // Whichever external libraries this build has, each name is parsed out
        // whole rather than left with the --enable- prefix attached.
        for (const std::string& name : info.external_libraries)
        {
            assert(name.find("--") == std::string::npos);
            assert(!name.empty());
        }

        // The parse has to agree with the configure line it came from.
        for (const std::string& name : info.external_libraries)
        {
            assert(info.configuration.find("--enable-" + name) != std::string::npos);
            assert(info.has_external_library(name));
        }

        assert(!info.has_external_library("lib-that-does-not-exist"));
    }

    void test_build_info_serializes()
    {
        const json::Value document = build_info().to_json();

        assert(document.at("build_id").get<std::string>() == build_info().build_id);
        assert(document.at("libraries").size() == 6);
        assert(!document.at("license").get<std::string>().empty());

        // Round-trips through the library's own serializer.
        assert(json::parse(document.dump()) == document);
    }
}

int main()
{
    test_build_info_reports_a_version_and_libraries();
    test_runtime_libraries_match_the_headers_we_compiled_against();
    test_the_build_id_is_stable_and_non_empty();
    test_the_license_is_identified();
    test_external_libraries_are_parsed_from_the_configure_line();
    test_build_info_serializes();
}
