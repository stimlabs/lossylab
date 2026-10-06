#include "lossylab/env/build_info.hpp"

#include "lossylab/core/json_io.hpp"
#include "lossylab/detail/ff_ptr.hpp"

#include "lossylab_build_config.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <span>

#include <lcms2.h>
#include <zlib.h>

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

        /// lcms2 encodes 2.14 as 2140.
        LibraryVersion lcms2_library_version()
        {
            const auto split = [](LibraryVersion& version, const int encoded, const bool compiled)
            {
                (compiled ? version.compiled_major : version.major) = encoded / 1000;
                (compiled ? version.compiled_minor : version.minor) = encoded % 1000 / 10;
                (compiled ? version.compiled_micro : version.micro) = encoded % 10;
            };
            LibraryVersion version;
            version.name = "lcms2";
            split(version, cmsGetEncodedCMMversion(), false);
            split(version, LCMS_VERSION, true);
            return version;
        }

        /// zlib reports its runtime version only as a string, "1.3" or
        /// "1.2.13"; ZLIB_VERNUM holds the compiled one as 0xMmrr.
        LibraryVersion zlib_library_version()
        {
            LibraryVersion version;
            version.name = "zlib";
            std::sscanf(zlibVersion(), "%d.%d.%d", &version.major, &version.minor, &version.micro);
            version.compiled_major = ZLIB_VERNUM >> 12;
            version.compiled_minor = (ZLIB_VERNUM >> 8) & 0xf;
            version.compiled_micro = (ZLIB_VERNUM >> 4) & 0xf;
            return version;
        }

        /// {"name": "major.minor.micro", ...}
        json::Value versions_by_name(const std::vector<LibraryVersion>& libraries)
        {
            json::Value versions = json::Value::object();
            for (const LibraryVersion& library : libraries)
            {
                versions[library.name] = std::to_string(library.major) + '.' + std::to_string(library.minor) + '.' +
                                         std::to_string(library.micro);
            }
            return versions;
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

        /// "sha256:" and the hex SHA-256 of `material`.
        std::string sha256(const std::string& material)
        {
            return detail::sha256_hex(
                std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(material.data()),
                                              material.size()));
        }

        FfmpegBuild compute_ffmpeg_build()
        {
            FfmpegBuild info;
            info.version = av_version_info() != nullptr ? av_version_info() : "unknown";
            info.configure_hash = sha256(avcodec_configuration() != nullptr ? avcodec_configuration() : "");
            info.license =
                parse_license(avcodec_license() != nullptr ? avcodec_license() : "");

            info.libraries = {
                make_version("libavutil", avutil_version(), LIBAVUTIL_VERSION_INT),
                make_version("libavcodec", avcodec_version(), LIBAVCODEC_VERSION_INT),
                make_version("libavformat", avformat_version(), LIBAVFORMAT_VERSION_INT),
                make_version("libavfilter", avfilter_version(), LIBAVFILTER_VERSION_INT),
                make_version("libswscale", swscale_version(), LIBSWSCALE_VERSION_INT),
                make_version("libswresample", swresample_version(), LIBSWRESAMPLE_VERSION_INT),
            };

            return info;
        }

        BuildInfo compute_build_info()
        {
            BuildInfo info;
            info.lossylab.commit = LOSSYLAB_GIT_COMMIT;
            info.lossylab.dirty = LOSSYLAB_GIT_DIRTY != 0;
            info.lossylab.compiler = LOSSYLAB_COMPILER;
            info.lossylab.build_type = LOSSYLAB_BUILD_TYPE;
            info.lossylab.libraries = {lcms2_library_version(), zlib_library_version()};
            info.ffmpeg = compute_ffmpeg_build();
            info.identity_hash = sha256(json::object({
                                                {"lossylab", info.lossylab.to_json()},
                                                {"ffmpeg", info.ffmpeg.to_json()},
                                            })
                                            .dump());
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

    bool FfmpegBuild::is_consistent() const noexcept
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

    json::Value LossylabBuild::to_json() const
    {
        return json::object({
            {"commit", commit},
            {"dirty", dirty},
            {"compiler", compiler},
            {"build_type", build_type},
            {"libraries", versions_by_name(libraries)},
        });
    }

    json::Value FfmpegBuild::to_json() const
    {
        return json::object({
            {"version", version},
            {"configure_hash", configure_hash},
            {"license", lossylab::to_string(license)},
            {"libraries", versions_by_name(libraries)},
        });
    }

    json::Value BuildInfo::to_json() const
    {
        return json::object({
            {"lossylab", lossylab.to_json()},
            {"ffmpeg", ffmpeg.to_json()},
            {"identity_hash", identity_hash},
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

    json::Value Diagnostics::to_json() const
    {
        return json::object({
            {"architecture", architecture},
            {"os", os},
            {"avx2", avx2},
        });
    }

    const Diagnostics& diagnostics()
    {
        static const Diagnostics info = []
        {
            Diagnostics result;
#if defined(__x86_64__) || defined(_M_X64)
            result.architecture = "x86_64";
            result.avx2 = __builtin_cpu_supports("avx2");
#elif defined(__aarch64__) || defined(_M_ARM64)
            result.architecture = "aarch64";
#else
            result.architecture = "unknown";
#endif
#if defined(__linux__)
            result.os = "linux";
#elif defined(__APPLE__)
            result.os = "macos";
#elif defined(_WIN32)
            result.os = "windows";
#else
            result.os = "unknown";
#endif
            return result;
        }();
        return info;
    }

    namespace
    {
        void collect_differences(const json::Value& a, const json::Value& b, const std::string& path,
                                 json::Value& differences)
        {
            const auto child_path = [&path](const std::string& name)
            { return path.empty() ? name : path + '.' + name; };
            if (a.is_object() && b.is_object())
            {
                for (const auto& [key, value] : a.items())
                {
                    collect_differences(value, json::member(b, key), child_path(key), differences);
                }
                for (const auto& [key, value] : b.items())
                {
                    if (!a.contains(key))
                    {
                        collect_differences(json::Value(), value, child_path(key), differences);
                    }
                }
            }
            else if (a.is_array() && b.is_array())
            {
                for (std::size_t index = 0; index < std::max(a.size(), b.size()); ++index)
                {
                    collect_differences(index < a.size() ? a[index] : json::Value(),
                                        index < b.size() ? b[index] : json::Value(), child_path(std::to_string(index)),
                                        differences);
                }
            }
            else if (a != b)
            {
                differences[path] = json::array({a, b});
            }
        }
    }

    json::Value build_diff(const json::Value& a, const json::Value& b)
    {
        json::Value differences = json::Value::object();
        collect_differences(a, b, "", differences);
        return differences;
    }
}
