#pragma once

#include "lossylab/core/json.hpp"
#include "lossylab/core/reflect.hpp"

#include <string>
#include <string_view>

namespace lossylab
{
    /// The enumerator values below deliberately equal FFmpeg's AVCol* / AVChroma*
    /// values. That keeps conversion free and, more importantly, keeps it from
    /// drifting: the source file checks these enums against the FFmpeg header in
    /// both directions, so neither a renumbering nor a value FFmpeg has and this
    /// list lacks can get past the build. No public header includes libavutil,
    /// so the values are restated here.

    enum class ColorMatrix : int
    {
        Rgb = 0,        ///< Identity; the frame is RGB, not YUV
        Bt709 = 1,
        Unspecified = 2,
        Fcc = 4,
        Bt470bg = 5,    ///< BT.601 625-line, "BT.601" in common usage
        Smpte170m = 6,  ///< BT.601 525-line
        Smpte240m = 7,
        Ycgco = 8,
        Bt2020Ncl = 9,
        Bt2020Cl = 10,
        Smpte2085 = 11,
        ChromaDerivedNcl = 12,
        ChromaDerivedCl = 13,
        Ictcp = 14,
        IptC2 = 15,     ///< SMPTE ST 2128
        YcgcoRe = 16,   ///< YCgCo-R, even bit addition
        YcgcoRo = 17    ///< YCgCo-R, odd bit addition
    };

    enum class ColorRange : int
    {
        Unspecified = 0,
        Limited = 1,  ///< "MPEG" / TV range: 16-235 luma at 8 bits
        Full = 2      ///< "JPEG" / PC range: 0-255
    };

    enum class ColorPrimaries : int
    {
        Bt709 = 1,
        Unspecified = 2,
        Bt470m = 4,
        Bt470bg = 5,
        Smpte170m = 6,
        Smpte240m = 7,
        Film = 8,
        Bt2020 = 9,
        Smpte428 = 10,
        Smpte431 = 11,  ///< DCI-P3
        Smpte432 = 12,  ///< Display P3
        Ebu3213 = 22
    };

    enum class TransferCharacteristic : int
    {
        Bt709 = 1,
        Unspecified = 2,
        Gamma22 = 4,
        Gamma28 = 5,
        Smpte170m = 6,
        Smpte240m = 7,
        Linear = 8,
        Log = 9,
        LogSqrt = 10,
        Iec61966_2_4 = 11,
        Bt1361Ecg = 12,
        Srgb = 13,       ///< IEC 61966-2-1
        Bt2020_10 = 14,
        Bt2020_12 = 15,
        Smpte2084 = 16,  ///< PQ
        Smpte428 = 17,
        AribStdB67 = 18  ///< HLG
    };

    /// Where chroma samples sit relative to luma. Getting this wrong shifts
    /// color by half a chroma sample, which is exactly the kind of trace the
    /// library is built to model and to detect.
    enum class ChromaLocation : int
    {
        Unspecified = 0,
        Left = 1,       ///< MPEG-2, H.264/HEVC default
        Center = 2,     ///< JPEG, MPEG-1, VP8/VP9
        TopLeft = 3,
        Top = 4,
        BottomLeft = 5,
        Bottom = 6
    };

    std::string to_string(ColorMatrix value);
    std::string to_string(ColorRange value);
    std::string to_string(ColorPrimaries value);
    std::string to_string(TransferCharacteristic value);
    std::string to_string(ChromaLocation value);

    ColorMatrix color_matrix_from_string(std::string_view name);
    ColorRange color_range_from_string(std::string_view name);
    ColorPrimaries color_primaries_from_string(std::string_view name);
    TransferCharacteristic transfer_from_string(std::string_view name);

    /// For reflect::from_json().
    inline void from_string(const std::string_view name, ColorPrimaries& value)
    {
        value = color_primaries_from_string(name);
    }

    inline void from_string(const std::string_view name, TransferCharacteristic& value)
    {
        value = transfer_from_string(name);
    }
    ChromaLocation chroma_location_from_string(std::string_view name);

    /// The complete color interpretation of a frame's samples.
    ///
    /// Every YUV<->RGB conversion in the library requires one explicitly.
    /// Leaving it implicit is how BT.601/BT.709 and limited/full mix-ups creep
    /// in, and those mix-ups are both an augmentation the library reproduces
    /// and a class-dependent trace it looks for.
    struct ColorSpec
    {
        ColorMatrix matrix = ColorMatrix::Unspecified;
        ColorRange range = ColorRange::Unspecified;
        ColorPrimaries primaries = ColorPrimaries::Unspecified;
        TransferCharacteristic transfer = TransferCharacteristic::Unspecified;
        ChromaLocation chroma_location = ChromaLocation::Unspecified;

        /// Named presets covering the combinations that actually occur.
        static ColorSpec bt709_limited() noexcept;
        static ColorSpec bt709_full() noexcept;
        static ColorSpec bt601_limited() noexcept;  ///< 625-line (BT.470BG)
        static ColorSpec bt601_full() noexcept;
        static ColorSpec smpte170m_limited() noexcept;  ///< 525-line
        static ColorSpec bt2020_ncl_limited() noexcept;
        static ColorSpec pq_bt2020() noexcept;   ///< HDR10
        static ColorSpec hlg_bt2020() noexcept;
        static ColorSpec srgb() noexcept;        ///< RGB identity matrix, full range
        static ColorSpec jpeg() noexcept;        ///< BT.601 full range, centered chroma

        /// True when no field is Unspecified. Conversions require this: a
        /// partially tagged spec means the result would depend on a default
        /// chosen somewhere out of sight.
        [[nodiscard]] bool is_fully_specified() const noexcept;

        /// Throws ConfigError naming the unspecified fields.
        void require_fully_specified(std::string_view context) const;

        /// Fills Unspecified fields from `fallback`, leaving set fields alone.
        /// Returns the fields that were filled, for the record.
        [[nodiscard]] ColorSpec with_defaults_from(const ColorSpec& fallback) const noexcept;

        /// True when the matrix is the RGB identity, i.e. samples are not YUV.
        [[nodiscard]] bool is_rgb() const noexcept;

        /// A short stable label such as "bt709/limited/bt709/bt709/left",
        /// used in conversion records and error messages.
        [[nodiscard]] std::string describe() const;

        [[nodiscard]] json::Value to_json() const;
        static ColorSpec from_json(const json::Value& value);
    };

    bool operator==(const ColorSpec& left, const ColorSpec& right) noexcept;
    inline bool operator!=(const ColorSpec& left, const ColorSpec& right) noexcept
    {
        return !(left == right);
    }

    LOSSYLAB_REFLECT(ColorSpec, matrix, range, primaries, transfer, chroma_location);
}
