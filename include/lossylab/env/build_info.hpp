#pragma once

#include "lossylab/core/json.hpp"
#include "lossylab/core/reflect.hpp"

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

    LOSSYLAB_REFLECT(LibraryVersion, name, major, minor, micro, compiled_major, compiled_minor, compiled_micro);

    /// The lossylab code itself: what produced the results, as opposed to the
    /// FFmpeg it runs on. Its algorithms (measure, compression_history) change
    /// results, so the commit is part of a build's identity.
    struct LossylabBuild
    {
        /// The git commit the library was built from.
        std::string commit;

        /// True when a tracked file differed from `commit` at build time, so
        /// the commit alone does not identify the code.
        bool dirty = false;

        /// The C++ compiler, e.g. "GNU 15.2.0". It changes the floating-point
        /// results of this library's own code.
        std::string compiler;

        /// "Release", "Debug", ...
        std::string build_type;

        [[nodiscard]] json::Value to_json() const;
    };

    LOSSYLAB_REFLECT(LossylabBuild, commit, dirty, compiler, build_type);

    /// Everything about the linked FFmpeg that can change an output byte.
    struct FfmpegBuild
    {
        /// FFmpeg's own version string, e.g. "n8.0.1".
        std::string version;

        /// "sha256:" and the SHA-256 of the ./configure command line FFmpeg
        /// was built with, which names every external library and hardware
        /// backend it enabled. It says whether two builds were configured
        /// alike, not how they differ.
        std::string configure_hash;

        License license = License::Unknown;

        /// `to_json()` writes these as {"libavcodec": "62.11.100", ...}.
        std::vector<LibraryVersion> libraries;

        /// True when every library's runtime version matches what the headers
        /// declared at compile time.
        [[nodiscard]] bool is_consistent() const noexcept;

        [[nodiscard]] json::Value to_json() const;
    };

    LOSSYLAB_REFLECT(FfmpegBuild, version, configure_hash, license, libraries);

    /// The identity of this build: the lossylab code and the FFmpeg it runs on.
    ///
    /// Every ProcessingRecord embeds it. Two records whose `identity_hash` is
    /// equal were produced by the same build; when it differs, `build_diff()`
    /// of the two identities names what changed, which is where to look when
    /// two records of the same input disagree.
    struct BuildInfo
    {
        LossylabBuild lossylab;
        FfmpegBuild ffmpeg;

        /// "sha256:" and the SHA-256 of `lossylab` and `ffmpeg`, as canonical
        /// JSON. A quick equality check between two builds, never a
        /// substitute for the fields themselves.
        std::string identity_hash;

        [[nodiscard]] json::Value to_json() const;
    };

    LOSSYLAB_REFLECT(BuildInfo, lossylab, ffmpeg, identity_hash);

    /// Describes this build. Computed once and cached; the result is pure
    /// data, so it is safe to call before a fork.
    [[nodiscard]] const BuildInfo& build_info();

    /// The machine a result was produced on. Recorded so a difference between
    /// two records can be blamed on it, and never part of a build's identity:
    /// results are expected to match across machines.
    struct Diagnostics
    {
        /// "x86_64", "aarch64", ...
        std::string architecture;

        /// "linux", "macos", "windows".
        std::string os;

        /// Whether the CPU has AVX2, which selects this library's vectorized
        /// paths and FFmpeg's.
        bool avx2 = false;

        [[nodiscard]] json::Value to_json() const;
    };

    LOSSYLAB_REFLECT(Diagnostics, architecture, os, avx2);

    [[nodiscard]] const Diagnostics& diagnostics();

    /// Every leaf that differs between two JSON documents (two builds'
    /// `to_json()`, say), keyed by its dotted path, e.g.
    /// "lossylab.commit" or "ffmpeg.libraries.libavcodec". Each entry is
    /// `[in_a, in_b]`, with null on the side where the leaf is absent. Empty
    /// when the documents are equal.
    [[nodiscard]] json::Value build_diff(const json::Value& a, const json::Value& b);
}
