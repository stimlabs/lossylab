#include "lossylab/core/color_spec.hpp"

#include "lossylab/core/error.hpp"

extern "C" {
#include <libavutil/pixdesc.h>
}

#include <array>
#include <vector>

namespace lossylab
{
    namespace
    {
        // The public enums restate FFmpeg's values so that no public header has
        // to include libavutil, which only works if the two stay in step. A
        // per-member assertion proves that every value the library names is the
        // right one, but on its own it says nothing about a value libavutil has
        // and this library does not: such a value reaches the public enum by a
        // cast and lands on no enumerator at all. So each enum is bound to
        // libavutil through one table, checked in both directions below.

        /// One library enumerator and the libavutil value it must equal.
        template <typename Enum>
        struct ValueBinding
        {
            Enum named;
            int libavutil_value;
        };

        template <typename Enum, std::size_t Count>
        constexpr bool values_match(const std::array<ValueBinding<Enum>, Count>& bindings)
        {
            for (const ValueBinding<Enum>& binding : bindings)
            {
                if (static_cast<int>(binding.named) != binding.libavutil_value)
                {
                    return false;
                }
            }
            return true;
        }

        /// True when every libavutil value below `libavutil_count` is either
        /// bound to an enumerator or listed as one libavutil reserves.
        ///
        /// The reserved list is written out rather than inferred, so that a code
        /// point libavutil later assigns cannot pass as a gap: it belongs to one
        /// list or the other, and adding it is a decision somebody makes.
        template <typename Enum, std::size_t Count, std::size_t ReservedCount>
        constexpr bool covers_libavutil(const std::array<ValueBinding<Enum>, Count>& bindings,
                                        const std::array<int, ReservedCount>& reserved,
                                        const int libavutil_count)
        {
            for (int value = 0; value < libavutil_count; ++value)
            {
                bool accounted_for = false;
                for (const ValueBinding<Enum>& binding : bindings)
                {
                    accounted_for = accounted_for || binding.libavutil_value == value;
                }
                for (const int reserved_value : reserved)
                {
                    accounted_for = accounted_for || reserved_value == value;
                }
                if (!accounted_for)
                {
                    return false;
                }
            }
            return true;
        }

        constexpr std::array<ValueBinding<ColorMatrix>, 17> color_matrix_bindings{{
            {ColorMatrix::Rgb, AVCOL_SPC_RGB},
            {ColorMatrix::Bt709, AVCOL_SPC_BT709},
            {ColorMatrix::Unspecified, AVCOL_SPC_UNSPECIFIED},
            {ColorMatrix::Fcc, AVCOL_SPC_FCC},
            {ColorMatrix::Bt470bg, AVCOL_SPC_BT470BG},
            {ColorMatrix::Smpte170m, AVCOL_SPC_SMPTE170M},
            {ColorMatrix::Smpte240m, AVCOL_SPC_SMPTE240M},
            {ColorMatrix::Ycgco, AVCOL_SPC_YCGCO},
            {ColorMatrix::Bt2020Ncl, AVCOL_SPC_BT2020_NCL},
            {ColorMatrix::Bt2020Cl, AVCOL_SPC_BT2020_CL},
            {ColorMatrix::Smpte2085, AVCOL_SPC_SMPTE2085},
            {ColorMatrix::ChromaDerivedNcl, AVCOL_SPC_CHROMA_DERIVED_NCL},
            {ColorMatrix::ChromaDerivedCl, AVCOL_SPC_CHROMA_DERIVED_CL},
            {ColorMatrix::Ictcp, AVCOL_SPC_ICTCP},
            {ColorMatrix::IptC2, AVCOL_SPC_IPT_C2},
            {ColorMatrix::YcgcoRe, AVCOL_SPC_YCGCO_RE},
            {ColorMatrix::YcgcoRo, AVCOL_SPC_YCGCO_RO},
        }};
        constexpr std::array<int, 1> reserved_color_matrices{{AVCOL_SPC_RESERVED}};

        constexpr std::array<ValueBinding<ColorRange>, 3> color_range_bindings{{
            {ColorRange::Unspecified, AVCOL_RANGE_UNSPECIFIED},
            {ColorRange::Limited, AVCOL_RANGE_MPEG},
            {ColorRange::Full, AVCOL_RANGE_JPEG},
        }};
        constexpr std::array<int, 0> reserved_color_ranges{};

        constexpr std::array<ValueBinding<ColorPrimaries>, 12> color_primaries_bindings{{
            {ColorPrimaries::Bt709, AVCOL_PRI_BT709},
            {ColorPrimaries::Unspecified, AVCOL_PRI_UNSPECIFIED},
            {ColorPrimaries::Bt470m, AVCOL_PRI_BT470M},
            {ColorPrimaries::Bt470bg, AVCOL_PRI_BT470BG},
            {ColorPrimaries::Smpte170m, AVCOL_PRI_SMPTE170M},
            {ColorPrimaries::Smpte240m, AVCOL_PRI_SMPTE240M},
            {ColorPrimaries::Film, AVCOL_PRI_FILM},
            {ColorPrimaries::Bt2020, AVCOL_PRI_BT2020},
            {ColorPrimaries::Smpte428, AVCOL_PRI_SMPTE428},
            {ColorPrimaries::Smpte431, AVCOL_PRI_SMPTE431},
            {ColorPrimaries::Smpte432, AVCOL_PRI_SMPTE432},
            {ColorPrimaries::Ebu3213, AVCOL_PRI_EBU3213},
        }};

        // 0 and 3 are reserved by name; 13 through 21 are ITU code points
        // libavutil counts but does not define, EBU3213 sitting alone at 22.
        constexpr std::array<int, 11> reserved_color_primaries{{
            AVCOL_PRI_RESERVED0, AVCOL_PRI_RESERVED, 13, 14, 15, 16, 17, 18, 19, 20, 21,
        }};

        constexpr std::array<ValueBinding<TransferCharacteristic>, 17> transfer_bindings{{
            {TransferCharacteristic::Bt709, AVCOL_TRC_BT709},
            {TransferCharacteristic::Unspecified, AVCOL_TRC_UNSPECIFIED},
            {TransferCharacteristic::Gamma22, AVCOL_TRC_GAMMA22},
            {TransferCharacteristic::Gamma28, AVCOL_TRC_GAMMA28},
            {TransferCharacteristic::Smpte170m, AVCOL_TRC_SMPTE170M},
            {TransferCharacteristic::Smpte240m, AVCOL_TRC_SMPTE240M},
            {TransferCharacteristic::Linear, AVCOL_TRC_LINEAR},
            {TransferCharacteristic::Log, AVCOL_TRC_LOG},
            {TransferCharacteristic::LogSqrt, AVCOL_TRC_LOG_SQRT},
            {TransferCharacteristic::Iec61966_2_4, AVCOL_TRC_IEC61966_2_4},
            {TransferCharacteristic::Bt1361Ecg, AVCOL_TRC_BT1361_ECG},
            {TransferCharacteristic::Srgb, AVCOL_TRC_IEC61966_2_1},
            {TransferCharacteristic::Bt2020_10, AVCOL_TRC_BT2020_10},
            {TransferCharacteristic::Bt2020_12, AVCOL_TRC_BT2020_12},
            {TransferCharacteristic::Smpte2084, AVCOL_TRC_SMPTE2084},
            {TransferCharacteristic::Smpte428, AVCOL_TRC_SMPTE428},
            {TransferCharacteristic::AribStdB67, AVCOL_TRC_ARIB_STD_B67},
        }};
        constexpr std::array<int, 2> reserved_transfers{{
            AVCOL_TRC_RESERVED0,
            AVCOL_TRC_RESERVED,
        }};

        constexpr std::array<ValueBinding<ChromaLocation>, 7> chroma_location_bindings{{
            {ChromaLocation::Unspecified, AVCHROMA_LOC_UNSPECIFIED},
            {ChromaLocation::Left, AVCHROMA_LOC_LEFT},
            {ChromaLocation::Center, AVCHROMA_LOC_CENTER},
            {ChromaLocation::TopLeft, AVCHROMA_LOC_TOPLEFT},
            {ChromaLocation::Top, AVCHROMA_LOC_TOP},
            {ChromaLocation::BottomLeft, AVCHROMA_LOC_BOTTOMLEFT},
            {ChromaLocation::Bottom, AVCHROMA_LOC_BOTTOM},
        }};
        constexpr std::array<int, 0> reserved_chroma_locations{};

        // Direction one: every value the library names is the value libavutil
        // gives it.
        static_assert(values_match(color_matrix_bindings));
        static_assert(values_match(color_range_bindings));
        static_assert(values_match(color_primaries_bindings));
        static_assert(values_match(transfer_bindings));
        static_assert(values_match(chroma_location_bindings));

        // Direction two: every value libavutil defines has somewhere to land.
        // This is the half that catches an omission, which a per-member
        // assertion cannot: it fires when libavutil gains a value, and equally
        // when the library's own list was incomplete from the start.
        static_assert(covers_libavutil(color_matrix_bindings, reserved_color_matrices,
                                       AVCOL_SPC_NB),
                      "libavutil defines a color space ColorMatrix does not name");
        static_assert(covers_libavutil(color_range_bindings, reserved_color_ranges,
                                       AVCOL_RANGE_NB),
                      "libavutil defines a color range ColorRange does not name");
        static_assert(covers_libavutil(color_primaries_bindings, reserved_color_primaries,
                                       AVCOL_PRI_NB),
                      "libavutil defines color primaries ColorPrimaries does not name");
        static_assert(covers_libavutil(transfer_bindings, reserved_transfers, AVCOL_TRC_NB),
                      "libavutil defines a transfer TransferCharacteristic does not name");
        static_assert(covers_libavutil(chroma_location_bindings, reserved_chroma_locations,
                                       AVCHROMA_LOC_NB),
                      "libavutil defines a chroma location ChromaLocation does not name");
    }

    namespace
    {
        /// FFmpeg owns the canonical spelling of every color enumerator, so the
        /// names are taken from it rather than duplicated in a local table.
        std::string name_or(const char* name, const char* fallback)
        {
            return name != nullptr ? std::string(name) : std::string(fallback);
        }

        /// Resolves a name by exact match against the enumerators FFmpeg knows.
        ///
        /// FFmpeg's own av_*_from_name helpers accept loose input, which would
        /// let a typo such as "bt709ish" resolve to something plausible. Silent
        /// mislabeling of color is precisely the failure this library exists to
        /// detect, so an unrecognized name is an error rather than a guess.
        template <typename NameFn>
        int exact_from_name(const std::string_view name, NameFn name_of, const int count,
                            const char* what)
        {
            for (int value = 0; value < count; ++value)
            {
                const char* candidate = name_of(value);
                if (candidate != nullptr && name == candidate)
                {
                    return value;
                }
            }
            throw ConfigError("unknown " + std::string(what) + " '" + std::string(name) + "'");
        }
    }

    std::string to_string(const ColorMatrix value)
    {
        return name_or(av_color_space_name(static_cast<AVColorSpace>(value)), "unspecified");
    }

    std::string to_string(const ColorRange value)
    {
        return name_or(av_color_range_name(static_cast<AVColorRange>(value)), "unspecified");
    }

    std::string to_string(const ColorPrimaries value)
    {
        return name_or(av_color_primaries_name(static_cast<AVColorPrimaries>(value)), "unspecified");
    }

    std::string to_string(const TransferCharacteristic value)
    {
        return name_or(av_color_transfer_name(static_cast<AVColorTransferCharacteristic>(value)),
                       "unspecified");
    }

    std::string to_string(const ChromaLocation value)
    {
        return name_or(av_chroma_location_name(static_cast<AVChromaLocation>(value)), "unspecified");
    }

    ColorMatrix color_matrix_from_string(const std::string_view name)
    {
        return static_cast<ColorMatrix>(exact_from_name(
            name,
            [](const int value) { return av_color_space_name(static_cast<AVColorSpace>(value)); },
            AVCOL_SPC_NB, "color matrix"));
    }

    ColorRange color_range_from_string(const std::string_view name)
    {
        // "limited" and "full" are the library's spelling; FFmpeg calls them
        // "tv"/"mpeg" and "pc"/"jpeg". Specs get written by hand, so both work.
        if (name == "limited") { return ColorRange::Limited; }
        if (name == "full") { return ColorRange::Full; }
        return static_cast<ColorRange>(exact_from_name(
            name,
            [](const int value) { return av_color_range_name(static_cast<AVColorRange>(value)); },
            AVCOL_RANGE_NB, "color range"));
    }

    ColorPrimaries color_primaries_from_string(const std::string_view name)
    {
        return static_cast<ColorPrimaries>(exact_from_name(
            name,
            [](const int value) { return av_color_primaries_name(static_cast<AVColorPrimaries>(value)); },
            AVCOL_PRI_NB, "color primaries"));
    }

    TransferCharacteristic transfer_from_string(const std::string_view name)
    {
        // FFmpeg spells the sRGB transfer by its standard number; "srgb" is the
        // name everyone actually uses for it.
        if (name == "srgb") { return TransferCharacteristic::Srgb; }
        return static_cast<TransferCharacteristic>(exact_from_name(
            name,
            [](const int value)
            {
                return av_color_transfer_name(static_cast<AVColorTransferCharacteristic>(value));
            },
            AVCOL_TRC_NB, "transfer characteristic"));
    }

    ChromaLocation chroma_location_from_string(const std::string_view name)
    {
        return static_cast<ChromaLocation>(exact_from_name(
            name,
            [](const int value) { return av_chroma_location_name(static_cast<AVChromaLocation>(value)); },
            AVCHROMA_LOC_NB, "chroma location"));
    }

    // -----------------------------------------------------------------------
    // Presets
    // -----------------------------------------------------------------------

    ColorSpec ColorSpec::bt709_limited() noexcept
    {
        return {ColorMatrix::Bt709, ColorRange::Limited, ColorPrimaries::Bt709,
                TransferCharacteristic::Bt709, ChromaLocation::Left};
    }

    ColorSpec ColorSpec::bt709_full() noexcept
    {
        ColorSpec spec = bt709_limited();
        spec.range = ColorRange::Full;
        return spec;
    }

    ColorSpec ColorSpec::bt601_limited() noexcept
    {
        return {ColorMatrix::Bt470bg, ColorRange::Limited, ColorPrimaries::Bt470bg,
                TransferCharacteristic::Smpte170m, ChromaLocation::Left};
    }

    ColorSpec ColorSpec::bt601_full() noexcept
    {
        ColorSpec spec = bt601_limited();
        spec.range = ColorRange::Full;
        return spec;
    }

    ColorSpec ColorSpec::smpte170m_limited() noexcept
    {
        return {ColorMatrix::Smpte170m, ColorRange::Limited, ColorPrimaries::Smpte170m,
                TransferCharacteristic::Smpte170m, ChromaLocation::Left};
    }

    ColorSpec ColorSpec::bt2020_ncl_limited() noexcept
    {
        return {ColorMatrix::Bt2020Ncl, ColorRange::Limited, ColorPrimaries::Bt2020,
                TransferCharacteristic::Bt2020_10, ChromaLocation::TopLeft};
    }

    ColorSpec ColorSpec::pq_bt2020() noexcept
    {
        return {ColorMatrix::Bt2020Ncl, ColorRange::Limited, ColorPrimaries::Bt2020,
                TransferCharacteristic::Smpte2084, ChromaLocation::TopLeft};
    }

    ColorSpec ColorSpec::hlg_bt2020() noexcept
    {
        return {ColorMatrix::Bt2020Ncl, ColorRange::Limited, ColorPrimaries::Bt2020,
                TransferCharacteristic::AribStdB67, ChromaLocation::TopLeft};
    }

    ColorSpec ColorSpec::srgb() noexcept
    {
        return {ColorMatrix::Rgb, ColorRange::Full, ColorPrimaries::Bt709,
                TransferCharacteristic::Srgb, ChromaLocation::Unspecified};
    }

    ColorSpec ColorSpec::jpeg() noexcept
    {
        return {ColorMatrix::Bt470bg, ColorRange::Full, ColorPrimaries::Bt470bg,
                TransferCharacteristic::Smpte170m, ChromaLocation::Center};
    }

    // -----------------------------------------------------------------------
    // Queries
    // -----------------------------------------------------------------------

    bool ColorSpec::is_rgb() const noexcept
    {
        return matrix == ColorMatrix::Rgb;
    }

    bool ColorSpec::is_fully_specified() const noexcept
    {
        if (matrix == ColorMatrix::Unspecified || range == ColorRange::Unspecified ||
            primaries == ColorPrimaries::Unspecified ||
            transfer == TransferCharacteristic::Unspecified)
        {
            return false;
        }
        // RGB has no chroma planes, so chroma siting is meaningfully absent
        // rather than missing.
        return is_rgb() || chroma_location != ChromaLocation::Unspecified;
    }

    void ColorSpec::require_fully_specified(const std::string_view context) const
    {
        if (is_fully_specified())
        {
            return;
        }

        std::vector<std::string> missing;
        if (matrix == ColorMatrix::Unspecified) { missing.emplace_back("matrix"); }
        if (range == ColorRange::Unspecified) { missing.emplace_back("range"); }
        if (primaries == ColorPrimaries::Unspecified) { missing.emplace_back("primaries"); }
        if (transfer == TransferCharacteristic::Unspecified) { missing.emplace_back("transfer"); }
        if (!is_rgb() && chroma_location == ChromaLocation::Unspecified)
        {
            missing.emplace_back("chroma_location");
        }

        std::string list;
        for (std::size_t i = 0; i < missing.size(); ++i)
        {
            if (i != 0) { list += ", "; }
            list += missing[i];
        }
        throw ConfigError(std::string(context) + " requires a fully specified ColorSpec; " +
                          "unspecified: " + list);
    }

    ColorSpec ColorSpec::with_defaults_from(const ColorSpec& fallback) const noexcept
    {
        ColorSpec result = *this;
        if (result.matrix == ColorMatrix::Unspecified) { result.matrix = fallback.matrix; }
        if (result.range == ColorRange::Unspecified) { result.range = fallback.range; }
        if (result.primaries == ColorPrimaries::Unspecified)
        {
            result.primaries = fallback.primaries;
        }
        if (result.transfer == TransferCharacteristic::Unspecified)
        {
            result.transfer = fallback.transfer;
        }
        if (result.chroma_location == ChromaLocation::Unspecified)
        {
            result.chroma_location = fallback.chroma_location;
        }
        return result;
    }

    std::string ColorSpec::describe() const
    {
        return to_string(matrix) + "/" + to_string(range) + "/" + to_string(primaries) + "/" +
               to_string(transfer) + "/" + to_string(chroma_location);
    }

    json::Value ColorSpec::to_json() const
    {
        return json::object({
            {"matrix", to_string(matrix)},
            {"range", to_string(range)},
            {"primaries", to_string(primaries)},
            {"transfer", to_string(transfer)},
            {"chroma_location", to_string(chroma_location)},
        });
    }

    ColorSpec ColorSpec::from_json(const json::Value& value)
    {
        ColorSpec spec;
        spec.matrix = color_matrix_from_string(value.at("matrix").get<std::string>());
        spec.range = color_range_from_string(value.at("range").get<std::string>());
        spec.primaries = color_primaries_from_string(value.at("primaries").get<std::string>());
        spec.transfer = transfer_from_string(value.at("transfer").get<std::string>());
        spec.chroma_location =
            chroma_location_from_string(value.at("chroma_location").get<std::string>());
        return spec;
    }

    bool operator==(const ColorSpec& left, const ColorSpec& right) noexcept
    {
        return left.matrix == right.matrix && left.range == right.range && left.primaries == right.primaries &&
               left.transfer == right.transfer && left.chroma_location == right.chroma_location;
    }
}
