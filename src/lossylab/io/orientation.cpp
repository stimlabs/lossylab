#include "lossylab/io/orientation.hpp"

#include "lossylab/core/error.hpp"

extern "C" {
#include <libavcodec/exif.h>
#include <libavutil/display.h>
#include <libavutil/frame.h>
#include <libavutil/pixdesc.h>
}

#include <cmath>
#include <cstring>
#include <string>

namespace lossylab::detail
{
    namespace
    {
        /// Where the pixel at (x, y) of a `width` x `height` image lands when
        /// the image is shown in `orientation`, per the EXIF definitions.
        struct Destination
        {
            int x = 0;
            int y = 0;
        };

        Destination oriented_position(const int orientation, const int x, const int y, const int width,
                                      const int height) noexcept
        {
            switch (orientation)
            {
            case 2: return {width - 1 - x, y};
            case 3: return {width - 1 - x, height - 1 - y};
            case 4: return {x, height - 1 - y};
            case 5: return {y, x};
            case 6: return {height - 1 - y, x};
            case 7: return {height - 1 - y, width - 1 - x};
            case 8: return {y, width - 1 - x};
            default: return {x, y};
            }
        }

        /// Bytes one pixel occupies on `plane`: the largest step of the
        /// components stored there.
        int pixel_bytes_on_plane(const AVPixFmtDescriptor& descriptor, const int plane)
        {
            int bytes = 0;
            for (int i = 0; i < descriptor.nb_components; ++i)
            {
                if (descriptor.comp[i].plane == plane)
                {
                    bytes = std::max(bytes, descriptor.comp[i].step);
                }
            }
            return bytes;
        }

        /// The format identical to `descriptor` but with the chroma
        /// subsampling's axes swapped, as a transposed 4:2:2 image (4:4:0)
        /// needs.
        std::optional<PixelFormat> transposed_format(const AVPixFmtDescriptor& descriptor)
        {
            const AVPixFmtDescriptor* candidate = nullptr;
            while ((candidate = av_pix_fmt_desc_next(candidate)) != nullptr)
            {
                if (candidate->log2_chroma_w != descriptor.log2_chroma_h ||
                    candidate->log2_chroma_h != descriptor.log2_chroma_w ||
                    candidate->nb_components != descriptor.nb_components || candidate->flags != descriptor.flags)
                {
                    continue;
                }
                bool same_components = true;
                for (int i = 0; i < descriptor.nb_components; ++i)
                {
                    const AVComponentDescriptor& expected = descriptor.comp[i];
                    const AVComponentDescriptor& actual = candidate->comp[i];
                    same_components = same_components && expected.plane == actual.plane &&
                                      expected.step == actual.step && expected.offset == actual.offset &&
                                      expected.shift == actual.shift && expected.depth == actual.depth;
                }
                if (same_components)
                {
                    return PixelFormat::from_raw(av_pix_fmt_desc_get_id(candidate));
                }
            }
            return std::nullopt;
        }
    }

    std::optional<int> exif_orientation(std::span<const std::uint8_t> exif)
    {
        constexpr std::string_view exif_prefix{"Exif\0\0", 6};
        const bool has_prefix =
            exif.size() >= exif_prefix.size() && std::memcmp(exif.data(), exif_prefix.data(), exif_prefix.size()) == 0;

        AVExifMetadata ifd{};
        if (av_exif_parse_buffer(nullptr, exif.data(), exif.size(), &ifd,
                                 has_prefix ? AV_EXIF_EXIF00 : AV_EXIF_TIFF_HEADER) < 0)
        {
            return std::nullopt;
        }

        std::optional<int> orientation;
        AVExifEntry* entry = nullptr;
        const std::int32_t orientation_tag = av_exif_get_tag_id("Orientation");
        if (orientation_tag >= 0 &&
            av_exif_get_entry(nullptr, &ifd, static_cast<std::uint16_t>(orientation_tag), 0, &entry) > 0 &&
            entry->count > 0 && entry->type == AV_TIFF_SHORT && entry->value.uint[0] >= 1 && entry->value.uint[0] <= 8)
        {
            orientation = static_cast<int>(entry->value.uint[0]);
        }
        av_exif_free(&ifd);
        return orientation;
    }

    std::optional<int> orientation_from_display_matrix(const std::span<const std::uint8_t> side_data)
    {
        if (side_data.size() < sizeof(std::int32_t) * 9)
        {
            return std::nullopt;
        }
        std::int32_t matrix[9];
        std::memcpy(matrix, side_data.data(), sizeof(matrix));

        const double rotation = av_display_rotation_get(matrix);
        if (!std::isfinite(rotation) || std::abs(std::remainder(rotation, 90.0)) > 1e-6)
        {
            return std::nullopt;
        }
        const int orientation = av_exif_matrix_to_orientation(matrix);
        return orientation >= 1 && orientation <= 8 ? std::optional<int>(orientation) : std::nullopt;
    }

    Frame apply_orientation(const Frame& frame, const int orientation, const Strict strict,
                            ConversionList& conversions)
    {
        const PixelFormat source_format = frame.pixel_format();
        const AVPixFmtDescriptor* descriptor = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(source_format.raw()));
        if (descriptor == nullptr)
        {
            throw ConfigError("apply_orientation() on a frame with no pixel format");
        }
        constexpr std::uint64_t unsupported_flags =
            AV_PIX_FMT_FLAG_BITSTREAM | AV_PIX_FMT_FLAG_HWACCEL | AV_PIX_FMT_FLAG_BAYER;
        const bool packed_and_subsampled = (descriptor->flags & AV_PIX_FMT_FLAG_PLANAR) == 0 &&
                                           (descriptor->log2_chroma_w != 0 || descriptor->log2_chroma_h != 0);
        if ((descriptor->flags & unsupported_flags) != 0 || packed_and_subsampled)
        {
            throw NotImplemented("applying an orientation to " + source_format.name());
        }

        const bool transposes = orientation_transposes(orientation);
        PixelFormat target_format = source_format;
        if (transposes && descriptor->log2_chroma_w != descriptor->log2_chroma_h)
        {
            const std::optional<PixelFormat> transposed = transposed_format(*descriptor);
            if (!transposed.has_value())
            {
                throw NotImplemented("applying a transposing orientation to " + source_format.name());
            }
            target_format = *transposed;
            record_or_refuse(strict, conversions, "subsampling", to_string(source_format.subsampling()),
                             to_string(target_format.subsampling()), ConversionCause::Requested, "lossylab",
                             "orientation");
            record_or_refuse(strict, conversions, "pix_fmt", source_format.name(), target_format.name(),
                             ConversionCause::Requested, "lossylab", "orientation");
        }

        const int width = transposes ? frame.height() : frame.width();
        const int height = transposes ? frame.width() : frame.height();
        Frame oriented = Frame::allocate(width, height, target_format, frame.color());
        oriented.set_time_base(frame.time_base());
        oriented.copy_embedded_from(frame);
        oriented.set_orientation(std::nullopt);
        if (transposes)
        {
            const Rational stored = frame.sample_aspect_ratio();
            oriented.set_sample_aspect_ratio(Rational{stored.den, stored.num});
        }

        for (int plane_index = 0; plane_index < frame.plane_count(); ++plane_index)
        {
            const ConstPlaneView source = frame.plane(plane_index);
            PlaneView target = oriented.plane(plane_index);
            const auto pixel_bytes = static_cast<std::size_t>(pixel_bytes_on_plane(*descriptor, plane_index));
            for (int y = 0; y < source.height; ++y)
            {
                const std::uint8_t* source_row = source.row(y);
                for (int x = 0; x < source.width; ++x)
                {
                    const Destination destination = oriented_position(orientation, x, y, source.width, source.height);
                    std::memcpy(target.row(destination.y) + static_cast<std::size_t>(destination.x) * pixel_bytes,
                                source_row + static_cast<std::size_t>(x) * pixel_bytes, pixel_bytes);
                }
            }
        }

        // A paletted frame's palette is not a plane of pixels and does not move.
        if ((descriptor->flags & AV_PIX_FMT_FLAG_PAL) != 0)
        {
            constexpr std::size_t palette_bytes = 256 * 4;
            std::memcpy(oriented.raw()->data[1], frame.raw()->data[1], palette_bytes);
        }
        return oriented;
    }
}
