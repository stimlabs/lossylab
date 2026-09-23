#include "lossylab/io/icc_profile.hpp"

#include "lossylab/core/json_io.hpp"

extern "C" {
#include <libavutil/md5.h>
}

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <map>
#include <string_view>

namespace lossylab
{
    namespace
    {
        using Xyz = std::array<double, 3>;
        using Matrix = std::array<std::array<double, 3>, 3>;

        constexpr std::size_t header_size = 128;
        constexpr Xyz d50_white = {0.9642, 1.0, 0.8249};

        /// Bytes of the profile, read big-endian with every access checked.
        class ProfileBytes
        {
        public:
            explicit ProfileBytes(const std::span<const std::uint8_t> bytes) : m_bytes(bytes) {}

            [[nodiscard]] std::size_t size() const noexcept { return m_bytes.size(); }

            [[nodiscard]] bool has(const std::size_t offset, const std::size_t count) const noexcept
            {
                return offset <= m_bytes.size() && count <= m_bytes.size() - offset;
            }

            [[nodiscard]] std::uint8_t u8(const std::size_t offset) const { return m_bytes[offset]; }

            [[nodiscard]] std::uint16_t u16(const std::size_t offset) const
            {
                return static_cast<std::uint16_t>((m_bytes[offset] << 8) | m_bytes[offset + 1]);
            }

            [[nodiscard]] std::uint32_t u32(const std::size_t offset) const
            {
                return (static_cast<std::uint32_t>(m_bytes[offset]) << 24) |
                       (static_cast<std::uint32_t>(m_bytes[offset + 1]) << 16) |
                       (static_cast<std::uint32_t>(m_bytes[offset + 2]) << 8) |
                       static_cast<std::uint32_t>(m_bytes[offset + 3]);
            }

            [[nodiscard]] double s15_fixed16(const std::size_t offset) const
            {
                return static_cast<double>(static_cast<std::int32_t>(u32(offset))) / 65536.0;
            }

            /// A four-character signature, without trailing spaces or NULs.
            /// Empty when all zero; hexadecimal when not printable.
            [[nodiscard]] std::string signature(const std::size_t offset) const
            {
                std::string text(reinterpret_cast<const char*>(m_bytes.data() + offset), 4);
                while (!text.empty() && (text.back() == ' ' || text.back() == '\0'))
                {
                    text.pop_back();
                }
                const bool printable =
                    std::all_of(text.begin(), text.end(), [](const char c) { return c >= 0x20 && c < 0x7f; });
                if (printable)
                {
                    return text;
                }
                char hex[9];
                std::snprintf(hex, sizeof(hex), "%08x", u32(offset));
                return hex;
            }

            [[nodiscard]] std::span<const std::uint8_t> slice(const std::size_t offset, const std::size_t count) const
            {
                return m_bytes.subspan(offset, count);
            }

        private:
            std::span<const std::uint8_t> m_bytes;
        };

        /// One tag's data, as the tag table places it.
        struct Tag
        {
            std::size_t offset = 0;
            std::size_t size = 0;
        };

        std::string device_class_name(const std::string& signature)
        {
            static const std::map<std::string, std::string, std::less<>> names = {
                {"scnr", "input"},       {"mntr", "display"},  {"prtr", "output"},      {"link", "device_link"},
                {"spac", "color_space"}, {"abst", "abstract"}, {"nmcl", "named_color"},
            };
            const auto it = names.find(signature);
            return it != names.end() ? it->second : signature;
        }

        void append_utf8(std::string& text, const std::uint32_t code_point)
        {
            if (code_point < 0x80)
            {
                text += static_cast<char>(code_point);
            }
            else if (code_point < 0x800)
            {
                text += static_cast<char>(0xc0 | (code_point >> 6));
                text += static_cast<char>(0x80 | (code_point & 0x3f));
            }
            else if (code_point < 0x10000)
            {
                text += static_cast<char>(0xe0 | (code_point >> 12));
                text += static_cast<char>(0x80 | ((code_point >> 6) & 0x3f));
                text += static_cast<char>(0x80 | (code_point & 0x3f));
            }
            else
            {
                text += static_cast<char>(0xf0 | (code_point >> 18));
                text += static_cast<char>(0x80 | ((code_point >> 12) & 0x3f));
                text += static_cast<char>(0x80 | ((code_point >> 6) & 0x3f));
                text += static_cast<char>(0x80 | (code_point & 0x3f));
            }
        }

        std::string utf16_big_endian_to_utf8(const ProfileBytes& bytes, const std::size_t offset,
                                             const std::size_t length)
        {
            std::string text;
            for (std::size_t at = offset; at + 1 < offset + length; at += 2)
            {
                std::uint32_t code_point = bytes.u16(at);
                const bool high_surrogate = code_point >= 0xd800 && code_point < 0xdc00;
                if (high_surrogate && at + 3 < offset + length)
                {
                    const std::uint32_t low = bytes.u16(at + 2);
                    if (low >= 0xdc00 && low < 0xe000)
                    {
                        code_point = 0x10000 + ((code_point - 0xd800) << 10) + (low - 0xdc00);
                        at += 2;
                    }
                }
                append_utf8(text, code_point);
            }
            return text;
        }

        std::string trim_nuls(std::string text)
        {
            const std::size_t end = text.find('\0');
            if (end != std::string::npos)
            {
                text.resize(end);
            }
            return text;
        }

        /// Text from a `desc` (version 2), `mluc` (version 4) or `text` tag.
        /// Of an `mluc` tag's translations, US English when present, else the
        /// first.
        std::optional<std::string> read_text(const ProfileBytes& bytes, const Tag& tag)
        {
            if (tag.size < 12)
            {
                return std::nullopt;
            }
            const std::string type = bytes.signature(tag.offset);
            if (type == "desc")
            {
                const std::size_t count = bytes.u32(tag.offset + 8);
                if (count > tag.size - 12)
                {
                    return std::nullopt;
                }
                const auto characters = bytes.slice(tag.offset + 12, count);
                return trim_nuls(std::string(characters.begin(), characters.end()));
            }
            if (type == "text")
            {
                const auto characters = bytes.slice(tag.offset + 8, tag.size - 8);
                return trim_nuls(std::string(characters.begin(), characters.end()));
            }
            if (type == "mluc" && tag.size >= 16)
            {
                const std::size_t record_count = bytes.u32(tag.offset + 8);
                const std::size_t record_size = bytes.u32(tag.offset + 12);
                if (record_size < 12 || record_count == 0 || record_count > (tag.size - 16) / record_size)
                {
                    return std::nullopt;
                }
                std::size_t chosen = 0;
                for (std::size_t i = 0; i < record_count; ++i)
                {
                    const std::size_t record = tag.offset + 16 + i * record_size;
                    if (bytes.signature(record) == "enUS")
                    {
                        chosen = i;
                        break;
                    }
                }
                const std::size_t record = tag.offset + 16 + chosen * record_size;
                const std::size_t length = bytes.u32(record + 4);
                const std::size_t offset = bytes.u32(record + 8);
                if (offset > tag.size || length > tag.size - offset)
                {
                    return std::nullopt;
                }
                return trim_nuls(utf16_big_endian_to_utf8(bytes, tag.offset + offset, length));
            }
            return std::nullopt;
        }

        std::optional<Xyz> read_xyz(const ProfileBytes& bytes, const Tag& tag)
        {
            if (tag.size < 20 || bytes.signature(tag.offset) != "XYZ")
            {
                return std::nullopt;
            }
            return Xyz{bytes.s15_fixed16(tag.offset + 8), bytes.s15_fixed16(tag.offset + 12),
                       bytes.s15_fixed16(tag.offset + 16)};
        }

        std::optional<Matrix> read_sf32_matrix(const ProfileBytes& bytes, const Tag& tag)
        {
            if (tag.size < 8 + 9 * 4 || bytes.signature(tag.offset) != "sf32")
            {
                return std::nullopt;
            }
            Matrix matrix{};
            for (std::size_t row = 0; row < 3; ++row)
            {
                for (std::size_t column = 0; column < 3; ++column)
                {
                    matrix[row][column] = bytes.s15_fixed16(tag.offset + 8 + (row * 3 + column) * 4);
                }
            }
            return matrix;
        }

        /// A tone curve from a `curv` or `para` tag, mapping encoded values in
        /// [0, 1] to linear light in [0, 1].
        struct Curve
        {
            enum class Kind
            {
                Gamma,
                Parametric,
                Sampled
            };
            Kind kind = Kind::Gamma;
            double gamma = 1.0;
            int function_type = 0;
            std::array<double, 7> parameters{};
            std::vector<double> samples;

            [[nodiscard]] double evaluate(const double x) const
            {
                switch (kind)
                {
                case Kind::Gamma: return std::pow(x, gamma);
                case Kind::Sampled:
                {
                    const double position = x * static_cast<double>(samples.size() - 1);
                    const auto below = static_cast<std::size_t>(std::floor(position));
                    const std::size_t above = std::min(below + 1, samples.size() - 1);
                    const double fraction = position - static_cast<double>(below);
                    return samples[below] + (samples[above] - samples[below]) * fraction;
                }
                case Kind::Parametric: break;
                }

                const double g = parameters[0];
                const double a = parameters[1];
                const double b = parameters[2];
                const double c = parameters[3];
                const double d = parameters[4];
                const double e = parameters[5];
                const double f = parameters[6];
                const auto power = [](const double base, const double exponent)
                { return base > 0.0 ? std::pow(base, exponent) : 0.0; };
                switch (function_type)
                {
                case 0: return power(x, g);
                case 1: return x >= -b / a ? power(a * x + b, g) : 0.0;
                case 2: return x >= -b / a ? power(a * x + b, g) + c : c;
                case 3: return x >= d ? power(a * x + b, g) : c * x;
                default: return x >= d ? power(a * x + b, g) + e : c * x + f;
                }
            }
        };

        std::optional<Curve> read_curve(const ProfileBytes& bytes, const Tag& tag)
        {
            if (tag.size < 12)
            {
                return std::nullopt;
            }
            const std::string type = bytes.signature(tag.offset);
            Curve curve;
            if (type == "curv")
            {
                const std::size_t count = bytes.u32(tag.offset + 8);
                if (count > (tag.size - 12) / 2)
                {
                    return std::nullopt;
                }
                if (count == 0)
                {
                    curve.gamma = 1.0;
                }
                else if (count == 1)
                {
                    curve.gamma = bytes.u16(tag.offset + 12) / 256.0;
                }
                else
                {
                    curve.kind = Curve::Kind::Sampled;
                    curve.samples.reserve(count);
                    for (std::size_t i = 0; i < count; ++i)
                    {
                        curve.samples.push_back(bytes.u16(tag.offset + 12 + i * 2) / 65535.0);
                    }
                }
                return curve;
            }
            if (type == "para")
            {
                constexpr std::array<std::size_t, 5> parameter_counts = {1, 3, 4, 5, 7};
                const int function_type = bytes.u16(tag.offset + 8);
                if (function_type < 0 || function_type > 4)
                {
                    return std::nullopt;
                }
                const std::size_t parameter_count = parameter_counts[static_cast<std::size_t>(function_type)];
                if (tag.size < 12 + parameter_count * 4)
                {
                    return std::nullopt;
                }
                curve.kind = Curve::Kind::Parametric;
                curve.function_type = function_type;
                for (std::size_t i = 0; i < parameter_count; ++i)
                {
                    curve.parameters[i] = bytes.s15_fixed16(tag.offset + 12 + i * 4);
                }
                if (function_type == 0)
                {
                    curve.kind = Curve::Kind::Gamma;
                    curve.gamma = curve.parameters[0];
                }
                return curve;
            }
            return std::nullopt;
        }

        double srgb_to_linear(const double value)
        {
            return value <= 0.04045 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
        }

        double bt709_to_linear(const double value)
        {
            return value < 0.081 ? value / 4.5 : std::pow((value + 0.099) / 1.099, 1.0 / 0.45);
        }

        /// Largest difference between two curves over [0, 1]. A 16-bit table
        /// sampling a curve stays well inside the tolerance it is compared
        /// against; sRGB and a pure 2.2 gamma differ by about 0.01.
        template <typename Reference>
        double largest_difference(const Curve& curve, Reference reference)
        {
            constexpr int steps = 256;
            double largest = 0.0;
            for (int i = 0; i <= steps; ++i)
            {
                const double x = static_cast<double>(i) / steps;
                largest = std::max(largest, std::abs(curve.evaluate(x) - reference(x)));
            }
            return largest;
        }

        /// A curve's classification, as `IccProfileInfo::transfer_curve` names
        /// it, with the exponent for a gamma curve.
        struct CurveClass
        {
            std::string name;
            std::optional<double> gamma;

            bool operator==(const CurveClass& other) const
            {
                if (name != other.name)
                {
                    return false;
                }
                return !gamma.has_value() || !other.gamma.has_value() || std::abs(*gamma - *other.gamma) < 0.01;
            }
        };

        CurveClass classify(const Curve& curve)
        {
            constexpr double tolerance = 0.002;
            constexpr double gamma_tolerance = 0.01;
            if (curve.kind == Curve::Kind::Gamma)
            {
                if (std::abs(curve.gamma - 1.0) < gamma_tolerance)
                {
                    return {"linear", std::nullopt};
                }
                return {"gamma", curve.gamma};
            }
            if (largest_difference(curve, srgb_to_linear) < tolerance)
            {
                return {"srgb", std::nullopt};
            }
            if (largest_difference(curve, bt709_to_linear) < tolerance)
            {
                return {"bt709", std::nullopt};
            }

            // A table or parametric curve that is a pure power function in
            // disguise; its exponent is read off the midpoint.
            const double midpoint = curve.evaluate(0.5);
            if (midpoint > 0.0 && midpoint < 1.0)
            {
                const double exponent = std::log(midpoint) / std::log(0.5);
                if (largest_difference(curve, [exponent](const double x) { return std::pow(x, exponent); }) <
                    tolerance)
                {
                    const double rounded = std::round(exponent * 100.0) / 100.0;
                    return std::abs(rounded - 1.0) < gamma_tolerance ? CurveClass{"linear", std::nullopt}
                                                                     : CurveClass{"gamma", rounded};
                }
            }
            return {curve.kind == Curve::Kind::Sampled ? "sampled" : "parametric", std::nullopt};
        }

        std::optional<TransferCharacteristic> transfer_for(const CurveClass& curve)
        {
            constexpr double gamma_tolerance = 0.01;
            if (curve.name == "srgb")
            {
                return TransferCharacteristic::Srgb;
            }
            if (curve.name == "bt709")
            {
                return TransferCharacteristic::Bt709;
            }
            if (curve.name == "linear")
            {
                return TransferCharacteristic::Linear;
            }
            if (curve.name == "gamma" && std::abs(*curve.gamma - 2.2) < gamma_tolerance)
            {
                return TransferCharacteristic::Gamma22;
            }
            if (curve.name == "gamma" && std::abs(*curve.gamma - 2.8) < gamma_tolerance)
            {
                return TransferCharacteristic::Gamma28;
            }
            return std::nullopt;
        }

        Matrix multiply(const Matrix& left, const Matrix& right)
        {
            Matrix product{};
            for (std::size_t row = 0; row < 3; ++row)
            {
                for (std::size_t column = 0; column < 3; ++column)
                {
                    for (std::size_t k = 0; k < 3; ++k)
                    {
                        product[row][column] += left[row][k] * right[k][column];
                    }
                }
            }
            return product;
        }

        Xyz multiply(const Matrix& matrix, const Xyz& vector)
        {
            Xyz result{};
            for (std::size_t row = 0; row < 3; ++row)
            {
                for (std::size_t k = 0; k < 3; ++k)
                {
                    result[row] += matrix[row][k] * vector[k];
                }
            }
            return result;
        }

        std::optional<Matrix> invert(const Matrix& m)
        {
            const double determinant = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) -
                                       m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
                                       m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
            if (std::abs(determinant) < 1e-12)
            {
                return std::nullopt;
            }
            return Matrix{{
                {(m[1][1] * m[2][2] - m[1][2] * m[2][1]) / determinant,
                 (m[0][2] * m[2][1] - m[0][1] * m[2][2]) / determinant,
                 (m[0][1] * m[1][2] - m[0][2] * m[1][1]) / determinant},
                {(m[1][2] * m[2][0] - m[1][0] * m[2][2]) / determinant,
                 (m[0][0] * m[2][2] - m[0][2] * m[2][0]) / determinant,
                 (m[0][2] * m[1][0] - m[0][0] * m[1][2]) / determinant},
                {(m[1][0] * m[2][1] - m[1][1] * m[2][0]) / determinant,
                 (m[0][1] * m[2][0] - m[0][0] * m[2][1]) / determinant,
                 (m[0][0] * m[1][1] - m[0][1] * m[1][0]) / determinant},
            }};
        }

        /// The Bradford transform from one white point to another, which is
        /// what version 2 profiles used to adapt their colorants to D50.
        Matrix bradford_adaptation(const Xyz& source_white, const Xyz& target_white)
        {
            constexpr Matrix bradford = {{
                {0.8951, 0.2664, -0.1614},
                {-0.7502, 1.7135, 0.0367},
                {0.0389, -0.0685, 1.0296},
            }};
            static const Matrix bradford_inverse = *invert(bradford);
            const Xyz source_cone = multiply(bradford, source_white);
            const Xyz target_cone = multiply(bradford, target_white);
            Matrix scale{};
            for (std::size_t i = 0; i < 3; ++i)
            {
                scale[i][i] = target_cone[i] / source_cone[i];
            }
            return multiply(bradford_inverse, multiply(scale, bradford));
        }

        Chromaticity chromaticity_of(const Xyz& xyz)
        {
            const double sum = xyz[0] + xyz[1] + xyz[2];
            if (sum <= 0.0)
            {
                return {};
            }
            return {xyz[0] / sum, xyz[1] / sum};
        }

        struct KnownPrimaries
        {
            std::string_view name;
            std::optional<ColorPrimaries> code;
            Chromaticity red;
            Chromaticity green;
            Chromaticity blue;
            Chromaticity white;
        };

        constexpr Chromaticity d65 = {0.3127, 0.3290};

        /// Ordered so that of two entries with the same values (SMPTE 170M
        /// and 240M) the first, more common one wins.
        const std::array<KnownPrimaries, 11>& known_primaries()
        {
            static const std::array<KnownPrimaries, 11> table = {{
                {"bt709", ColorPrimaries::Bt709, {0.640, 0.330}, {0.300, 0.600}, {0.150, 0.060}, d65},
                {"display_p3", ColorPrimaries::Smpte432, {0.680, 0.320}, {0.265, 0.690}, {0.150, 0.060}, d65},
                {"dci_p3", ColorPrimaries::Smpte431, {0.680, 0.320}, {0.265, 0.690}, {0.150, 0.060},
                 {0.314, 0.351}},
                {"bt2020", ColorPrimaries::Bt2020, {0.708, 0.292}, {0.170, 0.797}, {0.131, 0.046}, d65},
                {"bt470bg", ColorPrimaries::Bt470bg, {0.640, 0.330}, {0.290, 0.600}, {0.150, 0.060}, d65},
                {"smpte170m", ColorPrimaries::Smpte170m, {0.630, 0.340}, {0.310, 0.595}, {0.155, 0.070}, d65},
                {"bt470m", ColorPrimaries::Bt470m, {0.670, 0.330}, {0.210, 0.710}, {0.140, 0.080},
                 {0.310, 0.316}},
                {"ebu3213", ColorPrimaries::Ebu3213, {0.630, 0.340}, {0.295, 0.605}, {0.155, 0.077}, d65},
                {"adobe_rgb", std::nullopt, {0.640, 0.330}, {0.210, 0.710}, {0.150, 0.060}, d65},
                {"prophoto", std::nullopt, {0.7347, 0.2653}, {0.1596, 0.8404}, {0.0366, 0.0001},
                 {0.3457, 0.3585}},
                {"smpte240m", ColorPrimaries::Smpte240m, {0.630, 0.340}, {0.310, 0.595}, {0.155, 0.070}, d65},
            }};
            return table;
        }

        const KnownPrimaries* match_primaries(const IccProfileInfo::Colorants& colorants)
        {
            // Wide enough for s15Fixed16 rounding and the adaptation variants
            // profile makers use; narrower than the gap between any two
            // entries, the closest being BT.709 and BT.470BG's greens.
            constexpr double tolerance = 0.004;
            const auto near = [](const Chromaticity& measured, const Chromaticity& expected)
            { return std::abs(measured.x - expected.x) < tolerance && std::abs(measured.y - expected.y) < tolerance; };
            for (const KnownPrimaries& candidate : known_primaries())
            {
                if (near(colorants.red, candidate.red) && near(colorants.green, candidate.green) &&
                    near(colorants.blue, candidate.blue) && near(colorants.white, candidate.white))
                {
                    return &candidate;
                }
            }
            return nullptr;
        }

        /// The name a combination of primaries and tone curve is known by.
        std::string known_name(const std::string_view primaries, const CurveClass& curve)
        {
            const auto gamma_is = [&curve](const double expected)
            { return curve.name == "gamma" && std::abs(*curve.gamma - expected) < 0.01; };
            if (primaries == "bt709" && curve.name == "srgb")
            {
                return "sRGB";
            }
            if (primaries == "bt709" && curve.name == "bt709")
            {
                return "BT.709";
            }
            if (primaries == "display_p3" && curve.name == "srgb")
            {
                return "Display P3";
            }
            if (primaries == "dci_p3" && gamma_is(2.6))
            {
                return "DCI-P3";
            }
            if (primaries == "bt2020" && curve.name == "bt709")
            {
                return "BT.2020";
            }
            if (primaries == "adobe_rgb" && gamma_is(2.2))
            {
                return "Adobe RGB (1998)";
            }
            if (primaries == "prophoto" && gamma_is(1.8))
            {
                return "ProPhoto RGB";
            }
            return {};
        }

        std::string profile_id_hex(const std::array<std::uint8_t, 16>& digest)
        {
            std::string hex;
            for (const std::uint8_t byte : digest)
            {
                char pair[3];
                std::snprintf(pair, sizeof(pair), "%02x", byte);
                hex += pair;
            }
            return hex;
        }

        /// The profile ID as the ICC specification defines it: the MD5 of the
        /// profile with the flags, rendering intent and ID fields zeroed.
        std::array<std::uint8_t, 16> compute_profile_id(const std::span<const std::uint8_t> profile)
        {
            std::vector<std::uint8_t> copy(profile.begin(), profile.end());
            std::fill(copy.begin() + 44, copy.begin() + 48, 0);
            std::fill(copy.begin() + 64, copy.begin() + 68, 0);
            std::fill(copy.begin() + 84, copy.begin() + 100, 0);
            std::array<std::uint8_t, 16> digest{};
            av_md5_sum(digest.data(), copy.data(), copy.size());
            return digest;
        }

        std::string quoted(const std::string& signature) { return "'" + signature + "'"; }

        /// Reads the tone curves and colorants of a matrix/shaper profile.
        void read_matrix_shaper(const ProfileBytes& bytes, const std::map<std::string, Tag>& tags,
                                IccProfileInfo& info)
        {
            const auto find = [&tags](const std::string& signature) -> const Tag*
            {
                const auto it = tags.find(signature);
                return it != tags.end() ? &it->second : nullptr;
            };

            std::vector<std::string> curve_tags;
            if (info.data_color_space == "RGB")
            {
                curve_tags = {"rTRC", "gTRC", "bTRC"};
            }
            else if (info.data_color_space == "GRAY")
            {
                curve_tags = {"kTRC"};
            }
            else
            {
                return;
            }

            std::vector<CurveClass> classes;
            for (const std::string& signature : curve_tags)
            {
                const Tag* tag = find(signature);
                if (tag == nullptr)
                {
                    return;
                }
                const std::optional<Curve> curve = read_curve(bytes, *tag);
                if (!curve.has_value())
                {
                    info.problems.push_back("tag " + quoted(signature) + " is not a curve this parser reads");
                    return;
                }
                classes.push_back(classify(*curve));
            }

            const bool channels_agree =
                std::all_of(classes.begin(), classes.end(), [&classes](const CurveClass& curve)
                            { return curve == classes.front(); });
            const CurveClass curve = channels_agree ? classes.front() : CurveClass{"per_channel", std::nullopt};
            info.transfer_curve = curve.name;
            info.gamma = curve.gamma;
            info.transfer = transfer_for(curve);

            if (info.data_color_space == "GRAY")
            {
                info.is_matrix_shaper = true;
                return;
            }

            std::array<Xyz, 3> colorants{};
            constexpr std::array<std::string_view, 3> colorant_tags = {"rXYZ", "gXYZ", "bXYZ"};
            for (std::size_t i = 0; i < colorant_tags.size(); ++i)
            {
                const Tag* tag = find(std::string(colorant_tags[i]));
                if (tag == nullptr)
                {
                    return;
                }
                const std::optional<Xyz> xyz = read_xyz(bytes, *tag);
                if (!xyz.has_value())
                {
                    info.problems.push_back("tag " + quoted(std::string(colorant_tags[i])) + " is not an XYZ value");
                    return;
                }
                colorants[i] = *xyz;
            }
            info.is_matrix_shaper = true;

            // The colorants are stored adapted to D50. A `chad` tag states the
            // adaptation; without one, a version 2 profile adapted from its
            // media white point with the Bradford transform.
            Matrix undo_adaptation = {{{1.0, 0.0, 0.0}, {0.0, 1.0, 0.0}, {0.0, 0.0, 1.0}}};
            if (const Tag* chad = find("chad"))
            {
                const std::optional<Matrix> adaptation = read_sf32_matrix(bytes, *chad);
                const std::optional<Matrix> inverse =
                    adaptation.has_value() ? invert(*adaptation) : std::optional<Matrix>();
                if (inverse.has_value())
                {
                    undo_adaptation = *inverse;
                }
                else
                {
                    info.problems.push_back("tag 'chad' is not an invertible matrix");
                }
            }
            else if (const Tag* white_point = find("wtpt"))
            {
                const std::optional<Xyz> white = read_xyz(bytes, *white_point);
                const bool is_d50 = white.has_value() && std::abs((*white)[0] - d50_white[0]) < 0.002 &&
                                    std::abs((*white)[2] - d50_white[2]) < 0.002;
                if (white.has_value() && !is_d50)
                {
                    undo_adaptation = bradford_adaptation(d50_white, *white);
                }
            }

            Xyz white{};
            IccProfileInfo::Colorants measured;
            std::array<Chromaticity*, 3> targets = {&measured.red, &measured.green, &measured.blue};
            for (std::size_t i = 0; i < colorants.size(); ++i)
            {
                const Xyz source = multiply(undo_adaptation, colorants[i]);
                *targets[i] = chromaticity_of(source);
                for (std::size_t k = 0; k < 3; ++k)
                {
                    white[k] += source[k];
                }
            }
            measured.white = chromaticity_of(white);
            info.colorants = measured;

            if (const KnownPrimaries* known = match_primaries(measured))
            {
                info.primaries = known->code;
                info.known_as = known_name(known->name, curve);
            }
        }
    }

    json::Value Chromaticity::to_json() const { return json::object({{"x", x}, {"y", y}}); }

    json::Value IccProfileInfo::Colorants::to_json() const
    {
        return json::object({
            {"red", red.to_json()},
            {"green", green.to_json()},
            {"blue", blue.to_json()},
            {"white", white.to_json()},
        });
    }

    bool IccProfileInfo::is_expressible_as_tags() const noexcept
    {
        return primaries.has_value() && transfer.has_value();
    }

    std::optional<bool> IccProfileInfo::agrees_with(const ColorSpec& tagged) const noexcept
    {
        const bool tags_primaries = tagged.primaries != ColorPrimaries::Unspecified;
        const bool tags_transfer = tagged.transfer != TransferCharacteristic::Unspecified;
        if (!is_matrix_shaper || data_color_space != "RGB" || (!tags_primaries && !tags_transfer))
        {
            return std::nullopt;
        }
        return (!tags_primaries || primaries == tagged.primaries) && (!tags_transfer || transfer == tagged.transfer);
    }

    json::Value IccProfileInfo::to_json() const
    {
        return json::object({
            {"size_bytes", size_bytes},
            {"version", version},
            {"device_class", device_class},
            {"data_color_space", data_color_space},
            {"connection_space", connection_space},
            {"preferred_cmm", preferred_cmm},
            {"creator", creator},
            {"profile_id", profile_id},
            {"profile_id_embedded", profile_id_embedded},
            {"description", description},
            {"copyright", json::optional_or_null(copyright)},
            {"is_matrix_shaper", is_matrix_shaper},
            {"has_lookup_table", has_lookup_table},
            {"colorants", json::optional_or_null(colorants)},
            {"transfer_curve", transfer_curve},
            {"gamma", json::optional_or_null(gamma)},
            {"primaries", primaries.has_value() ? json::Value(to_string(*primaries)) : json::Value()},
            {"transfer", transfer.has_value() ? json::Value(to_string(*transfer)) : json::Value()},
            {"known_as", known_as},
            {"problems", json::to_array(problems)},
        });
    }

    IccProfileInfo describe_icc_profile(const std::span<const std::uint8_t> input)
    {
        IccProfileInfo info;
        info.size_bytes = static_cast<std::int64_t>(input.size());
        if (input.size() < header_size)
        {
            info.problems.push_back("shorter than the 128-byte header");
            return info;
        }

        ProfileBytes header(input);
        if (header.signature(36) != "acsp")
        {
            info.problems.push_back("no 'acsp' signature: not an ICC profile");
            return info;
        }

        // Some writers pad the profile; others were cut short in transit.
        const std::size_t declared_size = header.u32(0);
        std::span<const std::uint8_t> profile = input;
        if (declared_size < header_size)
        {
            info.problems.push_back("the header declares a size of " + std::to_string(declared_size) + " bytes");
        }
        else if (declared_size > input.size())
        {
            info.problems.push_back("truncated: the header declares " + std::to_string(declared_size) + " bytes, " +
                                    std::to_string(input.size()) + " are present");
        }
        else
        {
            profile = input.first(declared_size);
        }
        const ProfileBytes bytes(profile);

        info.preferred_cmm = bytes.signature(4);
        info.version = std::to_string(bytes.u8(8)) + "." + std::to_string(bytes.u8(9) >> 4);
        info.device_class = device_class_name(bytes.signature(12));
        info.data_color_space = bytes.signature(16);
        info.connection_space = bytes.signature(20);
        info.creator = bytes.signature(80);

        std::array<std::uint8_t, 16> embedded_id{};
        std::copy(profile.begin() + 84, profile.begin() + 100, embedded_id.begin());
        info.profile_id_embedded = std::any_of(embedded_id.begin(), embedded_id.end(),
                                               [](const std::uint8_t byte) { return byte != 0; });
        info.profile_id = profile_id_hex(info.profile_id_embedded ? embedded_id : compute_profile_id(profile));

        std::map<std::string, Tag> tags;
        if (!bytes.has(header_size, 4))
        {
            info.problems.push_back("the tag table is missing");
            return info;
        }
        const std::size_t tag_count = bytes.u32(header_size);
        if (tag_count > (bytes.size() - header_size - 4) / 12)
        {
            info.problems.push_back("the tag table runs past the end of the profile");
            return info;
        }
        for (std::size_t i = 0; i < tag_count; ++i)
        {
            const std::size_t entry = header_size + 4 + i * 12;
            const std::string signature = bytes.signature(entry);
            const Tag tag{bytes.u32(entry + 4), bytes.u32(entry + 8)};
            if (!bytes.has(tag.offset, tag.size))
            {
                info.problems.push_back("tag " + quoted(signature) + " lies outside the profile");
                continue;
            }
            tags.try_emplace(signature, tag);
        }

        if (const auto it = tags.find("desc"); it != tags.end())
        {
            info.description = read_text(bytes, it->second).value_or("");
        }
        if (const auto it = tags.find("cprt"); it != tags.end())
        {
            info.copyright = read_text(bytes, it->second);
        }
        info.has_lookup_table = tags.contains("A2B0");

        read_matrix_shaper(bytes, tags, info);
        return info;
    }
}
