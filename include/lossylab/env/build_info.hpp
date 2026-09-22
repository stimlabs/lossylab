#pragma once

#include "lossylab/core/json.hpp"

#include <string>
#include <vector>

namespace lossylab
{
    /// The license the linked FFmpeg was built under.
    ///
    /// Reported because it constrains redistribution: a GPL build cannot be
    /// shipped inside a closed-source product, and the answer depends on which
    /// external codecs were enabled, not on the library's own code.
    enum class License
    {
        Lgpl21,
        Lgpl3,
        Gpl2,
        Gpl3,
        Nonfree,
        Unknown
    };

    std::string to_string(License license);

    /// True when the license permits redistribution in a closed-source product.
    [[nodiscard]] bool permits_proprietary_distribution(License license);

    struct LibraryVersion
    {
        std::string name;
        int major = 0;
        int minor = 0;
        int micro = 0;

        /// The version the headers declared at compile time. A mismatch with
        /// the runtime version means the library was built against one FFmpeg
        /// and is running against another.
        int compiled_major = 0;
        int compiled_minor = 0;
        int compiled_micro = 0;

        [[nodiscard]] std::string to_string() const;
        [[nodiscard]] bool matches_compiled() const noexcept;
        [[nodiscard]] json::Value to_json() const;
    };

    /// Everything about the FFmpeg build that can change an output byte.
    ///
    /// Stored with experiment metadata and hashed into `build_id`, which every
    /// ProcessingRecord carries: two records that disagree on build id are not
    /// directly comparable, however similar their parameters look.
    struct BuildInfo
    {
        /// FFmpeg's own version string, e.g. "n8.0.1".
        std::string version;

        /// The full ./configure command line.
        std::string configuration;

        License license = License::Unknown;

        std::vector<LibraryVersion> libraries;

        /// External codec libraries this build enabled, parsed out of the
        /// configure flags: "libx264", "libaom", "libvmaf" and so on.
        std::vector<std::string> external_libraries;

        /// A short stable hash over the fields above. Identical builds produce
        /// identical ids; any change to versions or configure flags changes it.
        std::string build_id;

        /// True when every library's runtime version matches what the headers
        /// declared at compile time.
        [[nodiscard]] bool is_consistent() const noexcept;

        /// True when `name` appears in `external_libraries`.
        [[nodiscard]] bool has_external_library(std::string_view name) const noexcept;

        [[nodiscard]] json::Value to_json() const;
    };

    /// Describes the linked FFmpeg. Computed once and cached; the result is
    /// pure data, so it is safe to call before a fork.
    [[nodiscard]] const BuildInfo& build_info();
}
