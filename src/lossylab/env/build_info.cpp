#include "lossylab/env/build_info.hpp"

#include "lossylab/core/json_io.hpp"

#include <array>
#include <cstdio>
#include <sstream>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

namespace lossylab
{
    namespace
    {
        LibraryVersion make_version(const char* name, const unsigned runtime,
                                    const unsigned compiled)
        {
            LibraryVersion version;
            version.name = name;
            version.major = static_cast<int>(AV_VERSION_MAJOR(runtime));
            version.minor = static_cast<int>(AV_VERSION_MINOR(runtime));
            version.micro = static_cast<int>(AV_VERSION_MICRO(runtime));
            version.compiled_major = static_cast<int>(AV_VERSION_MAJOR(compiled));
            version.compiled_minor = static_cast<int>(AV_VERSION_MINOR(compiled));
            version.compiled_micro = static_cast<int>(AV_VERSION_MICRO(compiled));
            return version;
        }

        License parse_license(const std::string& text)
        {
            // avcodec_license() returns one of a fixed set of strings.
            if (text.find("nonfree") != std::string::npos) { return License::Nonfree; }
            if (text.find("GPL version 3") != std::string::npos) { return License::Gpl3; }
            if (text.find("LGPL version 3") != std::string::npos) { return License::Lgpl3; }
            if (text.find("GPL version 2") != std::string::npos) { return License::Gpl2; }
            if (text.find("LGPL version 2.1") != std::string::npos) { return License::Lgpl21; }
            return License::Unknown;
        }

        /// Pulls the enabled external libraries out of the configure line. They
        /// are what decides which codecs exist, so they belong in the record
        /// even though FFmpeg does not expose them as structured data.
        std::vector<std::string> parse_external_libraries(const std::string& configuration)
        {
            std::vector<std::string> libraries;
            std::istringstream stream(configuration);
            std::string flag;
            while (stream >> flag)
            {
                constexpr std::string_view enable = "--enable-";
                if (flag.compare(0, enable.size(), enable) != 0)
                {
                    continue;
                }
                const std::string name = flag.substr(enable.size());

                // External codec libraries, plus the hardware backends, which
                // decide what encoders exist just as much as the libraries do.
                if (name.compare(0, 3, "lib") == 0 || name == "vaapi" || name == "nvenc" ||
                    name == "cuda" || name == "cuvid" || name == "qsv" ||
                    name == "videotoolbox" || name == "vulkan")
                {
                    libraries.push_back(name);
                }
            }
            return libraries;
        }

        /// FNV-1a over the fields that can change an output byte. Short enough
        /// to read in a filename, wide enough not to collide in practice.
        std::string compute_build_id(const BuildInfo& info)
        {
            std::string material = info.version + '\n' + info.configuration + '\n';
            for (const LibraryVersion& library : info.libraries)
            {
                material += library.to_string() + '\n';
            }

            std::uint64_t hash = 1469598103934665603ULL;
            for (const char character : material)
            {
                hash ^= static_cast<unsigned char>(character);
                hash *= 1099511628211ULL;
            }

            std::array<char, 32> buffer{};
            std::snprintf(buffer.data(), buffer.size(), "%016llx",
                          static_cast<unsigned long long>(hash));
            return buffer.data();
        }

        BuildInfo compute_build_info()
        {
            BuildInfo info;
            info.version = av_version_info() != nullptr ? av_version_info() : "unknown";
            info.configuration =
                avcodec_configuration() != nullptr ? avcodec_configuration() : "";
            info.license =
                parse_license(avcodec_license() != nullptr ? avcodec_license() : "");
            info.external_libraries = parse_external_libraries(info.configuration);

            info.libraries = {
                make_version("libavutil", avutil_version(), LIBAVUTIL_VERSION_INT),
                make_version("libavcodec", avcodec_version(), LIBAVCODEC_VERSION_INT),
                make_version("libavformat", avformat_version(), LIBAVFORMAT_VERSION_INT),
                make_version("libavfilter", avfilter_version(), LIBAVFILTER_VERSION_INT),
                make_version("libswscale", swscale_version(), LIBSWSCALE_VERSION_INT),
                make_version("libswresample", swresample_version(), LIBSWRESAMPLE_VERSION_INT),
            };

            info.build_id = compute_build_id(info);
            return info;
        }
    }

    std::string to_string(const License license)
    {
        switch (license)
        {
        case License::Lgpl21: return "LGPL-2.1";
        case License::Lgpl3: return "LGPL-3.0";
        case License::Gpl2: return "GPL-2.0";
        case License::Gpl3: return "GPL-3.0";
        case License::Nonfree: return "nonfree";
        case License::Unknown: return "unknown";
        }
        return "unknown";
    }

    bool permits_proprietary_distribution(const License license)
    {
        switch (license)
        {
        case License::Lgpl21:
        case License::Lgpl3:
            return true;
        case License::Gpl2:
        case License::Gpl3:
        case License::Nonfree:
        case License::Unknown:
            // Unknown is treated as restrictive on purpose: guessing wrong in
            // the permissive direction is the expensive mistake.
            return false;
        }
        return false;
    }

    std::string LibraryVersion::to_string() const
    {
        return name + ' ' + std::to_string(major) + '.' + std::to_string(minor) + '.' +
               std::to_string(micro);
    }

    bool LibraryVersion::matches_compiled() const noexcept
    {
        return major == compiled_major && minor == compiled_minor && micro == compiled_micro;
    }

    json::Value LibraryVersion::to_json() const
    {
        return json::object({
            {"name", name},
            {"version", std::to_string(major) + '.' + std::to_string(minor) + '.' +
                            std::to_string(micro)},
            {"compiled_version", std::to_string(compiled_major) + '.' +
                                     std::to_string(compiled_minor) + '.' +
                                     std::to_string(compiled_micro)},
            {"matches_compiled", matches_compiled()},
        });
    }

    bool BuildInfo::is_consistent() const noexcept
    {
        for (const LibraryVersion& library : libraries)
        {
            if (!library.matches_compiled())
            {
                return false;
            }
        }
        return true;
    }

    bool BuildInfo::has_external_library(const std::string_view name) const noexcept
    {
        for (const std::string& library : external_libraries)
        {
            if (library == name)
            {
                return true;
            }
        }
        return false;
    }

    json::Value BuildInfo::to_json() const
    {
        return json::object({
            {"build_id", build_id},
            {"version", version},
            {"license", lossylab::to_string(license)},
            {"permits_proprietary_distribution", permits_proprietary_distribution(license)},
            {"consistent", is_consistent()},
            {"libraries", json::to_array(libraries)},
            {"external_libraries", json::to_array(external_libraries)},
            {"configuration", configuration},
        });
    }

    const BuildInfo& build_info()
    {
        // Function-local static: computed on first use, never at load time, and
        // pure data once built. Nothing here starts a thread or touches global
        // FFmpeg state, so a forked dataloader worker inherits it safely.
        static const BuildInfo info = compute_build_info();
        return info;
    }
}
