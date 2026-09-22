#include "lossylab/core/pixel_format.hpp"

#include "lossylab/core/error.hpp"

extern "C" {
#include <libavutil/pixdesc.h>
}

namespace lossylab
{
    namespace
    {
        const AVPixFmtDescriptor* describe(const int raw)
        {
            if (raw == AV_PIX_FMT_NONE)
            {
                return nullptr;
            }
            return av_pix_fmt_desc_get(static_cast<AVPixelFormat>(raw));
        }
    }

    std::string to_string(const Subsampling subsampling)
    {
        switch (subsampling)
        {
        case Subsampling::Rgb: return "rgb";
        case Subsampling::Gray: return "gray";
        case Subsampling::Yuv444: return "444";
        case Subsampling::Yuv440: return "440";
        case Subsampling::Yuv422: return "422";
        case Subsampling::Yuv420: return "420";
        case Subsampling::Yuv411: return "411";
        case Subsampling::Yuv410: return "410";
        }
        return "unknown";
    }

    Subsampling subsampling_from_string(const std::string_view name)
    {
        if (name == "rgb") { return Subsampling::Rgb; }
        if (name == "gray" || name == "400") { return Subsampling::Gray; }
        if (name == "444" || name == "4:4:4") { return Subsampling::Yuv444; }
        if (name == "440" || name == "4:4:0") { return Subsampling::Yuv440; }
        if (name == "422" || name == "4:2:2") { return Subsampling::Yuv422; }
        if (name == "420" || name == "4:2:0") { return Subsampling::Yuv420; }
        if (name == "411" || name == "4:1:1") { return Subsampling::Yuv411; }
        if (name == "410" || name == "4:1:0") { return Subsampling::Yuv410; }
        throw ConfigError("unknown subsampling '" + std::string(name) + "'");
    }

    PixelFormat::PixelFormat() noexcept : m_raw(AV_PIX_FMT_NONE) {}
    PixelFormat::PixelFormat(const int raw) noexcept : m_raw(raw) {}

    PixelFormat PixelFormat::from_raw(const int av_pix_fmt) noexcept
    {
        return PixelFormat(av_pix_fmt);
    }

    std::optional<PixelFormat> PixelFormat::find(const std::string_view name) noexcept
    {
        const AVPixelFormat fmt = av_get_pix_fmt(std::string(name).c_str());
        if (fmt == AV_PIX_FMT_NONE)
        {
            return std::nullopt;
        }
        return PixelFormat(fmt);
    }

    PixelFormat PixelFormat::from_name(const std::string_view name)
    {
        if (const std::optional<PixelFormat> found = find(name))
        {
            return *found;
        }
        throw ConfigError("unknown pixel format '" + std::string(name) + "'");
    }

    bool PixelFormat::is_valid() const noexcept
    {
        return describe(m_raw) != nullptr;
    }

    std::string PixelFormat::name() const
    {
        const char* name = av_get_pix_fmt_name(static_cast<AVPixelFormat>(m_raw));
        return name != nullptr ? std::string(name) : std::string("none");
    }

    int PixelFormat::bit_depth() const
    {
        const AVPixFmtDescriptor* descriptor = describe(m_raw);
        return descriptor != nullptr ? descriptor->comp[0].depth : 0;
    }

    int PixelFormat::component_count() const
    {
        const AVPixFmtDescriptor* descriptor = describe(m_raw);
        return descriptor != nullptr ? descriptor->nb_components : 0;
    }

    int PixelFormat::plane_count() const
    {
        if (!is_valid())
        {
            return 0;
        }
        return av_pix_fmt_count_planes(static_cast<AVPixelFormat>(m_raw));
    }

    int PixelFormat::log2_chroma_width() const
    {
        const AVPixFmtDescriptor* descriptor = describe(m_raw);
        return descriptor != nullptr ? descriptor->log2_chroma_w : 0;
    }

    int PixelFormat::log2_chroma_height() const
    {
        const AVPixFmtDescriptor* descriptor = describe(m_raw);
        return descriptor != nullptr ? descriptor->log2_chroma_h : 0;
    }

    bool PixelFormat::is_rgb() const
    {
        const AVPixFmtDescriptor* descriptor = describe(m_raw);
        return descriptor != nullptr && (descriptor->flags & AV_PIX_FMT_FLAG_RGB) != 0;
    }

    bool PixelFormat::is_gray() const
    {
        const AVPixFmtDescriptor* descriptor = describe(m_raw);
        if (descriptor == nullptr)
        {
            return false;
        }
        // Alpha does not make a format non-gray, so it is excluded from the count.
        const int color_components =
            descriptor->nb_components - ((descriptor->flags & AV_PIX_FMT_FLAG_ALPHA) != 0 ? 1 : 0);
        return color_components == 1;
    }

    bool PixelFormat::is_planar() const
    {
        const AVPixFmtDescriptor* descriptor = describe(m_raw);
        return descriptor != nullptr && (descriptor->flags & AV_PIX_FMT_FLAG_PLANAR) != 0;
    }

    bool PixelFormat::has_alpha() const
    {
        const AVPixFmtDescriptor* descriptor = describe(m_raw);
        return descriptor != nullptr && (descriptor->flags & AV_PIX_FMT_FLAG_ALPHA) != 0;
    }

    bool PixelFormat::is_big_endian() const
    {
        const AVPixFmtDescriptor* descriptor = describe(m_raw);
        return descriptor != nullptr && (descriptor->flags & AV_PIX_FMT_FLAG_BE) != 0;
    }

    Subsampling PixelFormat::subsampling() const
    {
        if (!is_valid())
        {
            throw ConfigError("subsampling queried on an invalid pixel format");
        }
        if (is_rgb())
        {
            return Subsampling::Rgb;
        }
        if (is_gray())
        {
            return Subsampling::Gray;
        }

        switch ((log2_chroma_width() << 4) | log2_chroma_height())
        {
        case 0x00: return Subsampling::Yuv444;
        case 0x01: return Subsampling::Yuv440;
        case 0x10: return Subsampling::Yuv422;
        case 0x11: return Subsampling::Yuv420;
        case 0x20: return Subsampling::Yuv411;
        case 0x22: return Subsampling::Yuv410;
        default:
            throw ConfigError("pixel format '" + name() + "' has no named subsampling");
        }
    }

    PixelFormat PixelFormat::planar_yuv(const Subsampling subsampling, const int bit_depth,
                                        const bool with_alpha)
    {
        // Built by name rather than by table: FFmpeg's naming is regular here,
        // and going through av_get_pix_fmt means a format this build does not
        // have is reported as such instead of being fabricated.
        std::string base;
        switch (subsampling)
        {
        case Subsampling::Gray: base = "gray"; break;
        case Subsampling::Yuv444: base = "yuv444p"; break;
        case Subsampling::Yuv440: base = "yuv440p"; break;
        case Subsampling::Yuv422: base = "yuv422p"; break;
        case Subsampling::Yuv420: base = "yuv420p"; break;
        case Subsampling::Yuv411: base = "yuv411p"; break;
        case Subsampling::Yuv410: base = "yuv410p"; break;
        case Subsampling::Rgb:
            throw ConfigError("planar_yuv() called with Subsampling::Rgb");
        }

        if (with_alpha)
        {
            if (subsampling == Subsampling::Gray)
            {
                throw ConfigError("no planar gray format with alpha");
            }
            base = "yuva" + base.substr(3);  // yuv420p -> yuva420p
        }

        std::string full = base;
        if (subsampling == Subsampling::Gray)
        {
            // gray, gray10le, gray12le: no "p", and 8-bit has no suffix.
            if (bit_depth != 8)
            {
                full += std::to_string(bit_depth) + "le";
            }
        }
        else if (bit_depth != 8)
        {
            full += std::to_string(bit_depth) + "le";
        }

        if (const std::optional<PixelFormat> found = find(full))
        {
            return *found;
        }
        throw ConfigError("no pixel format for " + to_string(subsampling) + " at " +
                          std::to_string(bit_depth) + " bits" +
                          (with_alpha ? " with alpha" : "") + " (tried '" + full + "')");
    }

    std::vector<PixelFormat> PixelFormat::all()
    {
        std::vector<PixelFormat> formats;
        const AVPixFmtDescriptor* descriptor = nullptr;
        while ((descriptor = av_pix_fmt_desc_next(descriptor)) != nullptr)
        {
            formats.push_back(PixelFormat(av_pix_fmt_desc_get_id(descriptor)));
        }
        return formats;
    }

    json::Value PixelFormat::to_json() const
    {
        return json::Value(name());
    }

    PixelFormat PixelFormat::from_json(const json::Value& value)
    {
        const std::string& name = value.get_ref<const std::string&>();
        if (name == "none")
        {
            return {};
        }
        return from_name(name);
    }

    bool operator==(const PixelFormat& left, const PixelFormat& right) noexcept
    {
        return left.raw() == right.raw();
    }
}
