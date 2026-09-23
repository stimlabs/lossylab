#pragma once

#include "lossylab/core/color_spec.hpp"
#include "lossylab/core/json.hpp"
#include "lossylab/core/reflect.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace lossylab
{
    /// A CIE 1931 xy chromaticity.
    struct Chromaticity
    {
        double x = 0.0;
        double y = 0.0;

        [[nodiscard]] json::Value to_json() const;
    };

    LOSSYLAB_REFLECT(Chromaticity, x, y);

    /// What an embedded ICC profile says about the file's color.
    ///
    /// Read from the profile's header and its tags, without a color management
    /// module. For a matrix/shaper RGB profile (the kind displays and most
    /// photos carry) the primaries, white point and tone curve are measured
    /// from the tags themselves, so a profile is recognized by what it
    /// describes rather than by what its description claims.
    struct IccProfileInfo
    {
        std::int64_t size_bytes = 0;

        /// "4.3", "2.1", ...
        std::string version;

        /// "display", "input", "output", "color_space", "abstract",
        /// "device_link" or "named_color"; the raw signature otherwise.
        std::string device_class;

        /// The color space of the data the profile applies to: "RGB",
        /// "GRAY", "CMYK", "YCbr", "Lab", ...
        std::string data_color_space;

        /// "XYZ" or "Lab".
        std::string connection_space;

        /// Signatures of the preferred color management module and of the
        /// profile's creator, e.g. "appl", "lcms"; empty when unset.
        std::string preferred_cmm;
        std::string creator;

        /// The MD5 profile ID as 32 hexadecimal digits. Identical for every
        /// copy of a well-known profile, whatever file carries it.
        std::string profile_id;

        /// False when the header left the ID zero, as version 2 profiles do,
        /// and `profile_id` was computed here the way the ICC specification
        /// defines it.
        bool profile_id_embedded = false;

        /// The description tag, verbatim, e.g. "Display P3" or "sRGB
        /// IEC61966-2.1". Written by whoever made the profile, so it is
        /// evidence of the profile's origin rather than of its content.
        std::string description;
        std::optional<std::string> copyright;

        /// True for an RGB profile defined by three colorants and three tone
        /// curves, or a gray profile defined by one tone curve.
        bool is_matrix_shaper = false;

        /// True when the profile carries a lookup-table transform (an A2B0
        /// tag), which is how printer, CMYK and camera profiles are built.
        bool has_lookup_table = false;

        /// For a matrix/shaper RGB profile: the primaries and white point,
        /// with the profile's chromatic adaptation to D50 undone.
        struct Colorants
        {
            Chromaticity red;
            Chromaticity green;
            Chromaticity blue;
            Chromaticity white;

            [[nodiscard]] json::Value to_json() const;
        };
        std::optional<Colorants> colorants;

        /// The tone curve: "srgb", "bt709", "gamma", "linear", "parametric"
        /// for any other parametric curve, "sampled" for a table matching no
        /// known curve, or "per_channel" when the channels' curves differ.
        /// Empty when the profile has none.
        std::string transfer_curve;

        /// The exponent, when `transfer_curve` is "gamma".
        std::optional<double> gamma;

        /// The colorants and tone curve expressed as the codes a file's color
        /// tags use, when they match one. Adobe RGB and ProPhoto have no such
        /// code; see `known_as`.
        std::optional<ColorPrimaries> primaries;
        std::optional<TransferCharacteristic> transfer;

        /// The color space the colorants and tone curve match: "sRGB",
        /// "BT.709", "Display P3", "DCI-P3", "BT.2020", "Adobe RGB (1998)",
        /// "ProPhoto RGB". Empty when they match none of these.
        std::string known_as;

        /// Structural problems found while reading: a truncated profile, a
        /// tag outside it, a missing signature. The fields above hold what
        /// could still be read.
        std::vector<std::string> problems;

        /// True when `primaries` and `transfer` are both known, so the profile
        /// can stand in for color tags a file left unspecified.
        [[nodiscard]] bool is_expressible_as_tags() const noexcept;

        /// Whether the profile describes the primaries and transfer `tagged`
        /// names; a field left unspecified agrees with anything. Nullopt when
        /// there is nothing to compare: the tags name neither field, or the
        /// profile is not an RGB matrix/shaper profile.
        [[nodiscard]] std::optional<bool> agrees_with(const ColorSpec& tagged) const noexcept;

        [[nodiscard]] json::Value to_json() const;
    };

    LOSSYLAB_REFLECT(IccProfileInfo, size_bytes, version, device_class, data_color_space, connection_space,
                      preferred_cmm, creator, profile_id, profile_id_embedded, description, copyright,
                      is_matrix_shaper, has_lookup_table, colorants, transfer_curve, gamma, primaries, transfer,
                      known_as, problems);

    /// Reads an ICC profile's header and tags.
    ///
    /// Never throws on malformed input: whatever can be read is returned and
    /// the rest is listed in `problems`, since a damaged profile is itself
    /// something an audit reports.
    [[nodiscard]] IccProfileInfo describe_icc_profile(std::span<const std::uint8_t> bytes);
}
