#pragma once

#include "lossylab/core/json.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lossylab
{
    /// Chroma subsampling, named independently of bit depth and layout.
    ///
    /// The design treats subsampling as a first-class axis, with
    /// `chroma_roundtrip` taking one directly, because a chroma history is both
    /// an augmentation the library applies and a trace it looks for.
    enum class Subsampling
    {
        Rgb,      ///< Not YUV at all
        Gray,     ///< Luma only
        Yuv444,
        Yuv440,
        Yuv422,
        Yuv420,
        Yuv411,
        Yuv410
    };

    std::string to_string(Subsampling subsampling);
    Subsampling subsampling_from_string(std::string_view name);

    /// For reflect::from_json().
    inline void from_string(const std::string_view name, Subsampling& value)
    {
        value = subsampling_from_string(name);
    }

    /// A pixel format, by name rather than by FFmpeg enum, so that no public
    /// header has to include libavutil.
    ///
    /// Every stage declares the format it produces. Nothing infers one.
    class PixelFormat
    {
    public:
        /// Constructs the "no format" value, which every query reports as invalid.
        PixelFormat() noexcept;

        /// Throws ConfigError when FFmpeg does not know the name.
        static PixelFormat from_name(std::string_view name);

        /// Returns nullopt instead of throwing.
        static std::optional<PixelFormat> find(std::string_view name) noexcept;

        /// Wraps a raw AVPixelFormat value. Used at the FFmpeg boundary.
        static PixelFormat from_raw(int av_pix_fmt) noexcept;
        [[nodiscard]] int raw() const noexcept { return m_raw; }

        [[nodiscard]] bool is_valid() const noexcept;

        /// FFmpeg's canonical name, or "none" when invalid.
        [[nodiscard]] std::string name() const;

        /// Bits per component, e.g. 8 for yuv420p, 10 for yuv420p10le.
        [[nodiscard]] int bit_depth() const;

        [[nodiscard]] int component_count() const;
        [[nodiscard]] int plane_count() const;

        /// Chroma plane size relative to luma, as a power of two: 1 means the
        /// chroma planes are half width (or height).
        [[nodiscard]] int log2_chroma_width() const;
        [[nodiscard]] int log2_chroma_height() const;

        [[nodiscard]] Subsampling subsampling() const;

        [[nodiscard]] bool is_rgb() const;
        [[nodiscard]] bool is_gray() const;
        [[nodiscard]] bool is_planar() const;
        [[nodiscard]] bool has_alpha() const;

        /// True when the samples are big-endian in memory.
        [[nodiscard]] bool is_big_endian() const;

        /// The canonical planar format for a subsampling and bit depth, e.g.
        /// (Yuv420, 10) gives yuv420p10le. Throws ConfigError when the
        /// combination does not exist.
        static PixelFormat planar_yuv(Subsampling subsampling, int bit_depth,
                                      bool with_alpha = false);

        /// Every pixel format FFmpeg knows, for capability reporting.
        [[nodiscard]] static std::vector<PixelFormat> all();

        [[nodiscard]] json::Value to_json() const;
        static PixelFormat from_json(const json::Value& value);

    private:
        explicit PixelFormat(int raw) noexcept;

        int m_raw;
    };

    bool operator==(const PixelFormat& left, const PixelFormat& right) noexcept;
    inline bool operator!=(const PixelFormat& left, const PixelFormat& right) noexcept
    {
        return !(left == right);
    }
}
