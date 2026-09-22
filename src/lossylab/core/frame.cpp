#include "lossylab/core/frame.hpp"

#include "lossylab/core/error.hpp"
#include "lossylab/detail/ff_error.hpp"
#include "lossylab/detail/ff_ptr.hpp"

#include <algorithm>
#include <cmath>

extern "C" {
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libavutil/video_enc_params.h>
}

namespace lossylab
{
    namespace
    {
        PictureType from_av_picture_type(const AVPictureType type)
        {
            switch (type)
            {
            case AV_PICTURE_TYPE_I: return PictureType::I;
            case AV_PICTURE_TYPE_P: return PictureType::P;
            case AV_PICTURE_TYPE_B: return PictureType::B;
            default: return PictureType::Unknown;
            }
        }

        ColorSpec color_from_av_frame(const AVFrame& frame)
        {
            ColorSpec color;
            color.matrix = static_cast<ColorMatrix>(frame.colorspace);
            color.range = static_cast<ColorRange>(frame.color_range);
            color.primaries = static_cast<ColorPrimaries>(frame.color_primaries);
            color.transfer = static_cast<TransferCharacteristic>(frame.color_trc);
            color.chroma_location = static_cast<ChromaLocation>(frame.chroma_location);
            return color;
        }
    }

    std::ptrdiff_t PlaneView::row_bytes() const noexcept
    {
        return static_cast<std::ptrdiff_t>(width) * bytes_per_sample * components_per_pixel;
    }

    std::uint8_t* PlaneView::row(const int index) const noexcept
    {
        return data + stride * index;
    }

    std::ptrdiff_t ConstPlaneView::row_bytes() const noexcept
    {
        return static_cast<std::ptrdiff_t>(width) * bytes_per_sample * components_per_pixel;
    }

    const std::uint8_t* ConstPlaneView::row(const int index) const noexcept
    {
        return data + stride * index;
    }

    // -----------------------------------------------------------------------
    // QpMap
    // -----------------------------------------------------------------------

    int QpMap::at(const int block_x, const int block_y) const
    {
        if (block_x < 0 || block_x >= width || block_y < 0 || block_y >= height)
        {
            throw ConfigError("QpMap index (" + std::to_string(block_x) + ", " +
                              std::to_string(block_y) + ") is outside a " +
                              std::to_string(width) + "x" + std::to_string(height) + " map");
        }
        return values[static_cast<std::size_t>(block_y) * static_cast<std::size_t>(width) +
                      static_cast<std::size_t>(block_x)];
    }

    double QpMap::mean_over(const Rect& pixels) const
    {
        if (values.empty() || block_width <= 0 || block_height <= 0)
        {
            throw ConfigError("mean_over() on an empty QpMap");
        }

        // Every block the rectangle touches counts, including partially covered
        // ones: a crop straddling a block boundary carries that block's
        // quantization whether or not it covers all of it.
        const int first_x = std::max(0, static_cast<int>(std::floor(pixels.x / block_width)));
        const int first_y = std::max(0, static_cast<int>(std::floor(pixels.y / block_height)));
        const int last_x = std::min(
            width - 1,
            static_cast<int>(std::floor((pixels.x + pixels.width - 1e-9) / block_width)));
        const int last_y = std::min(
            height - 1,
            static_cast<int>(std::floor((pixels.y + pixels.height - 1e-9) / block_height)));

        if (last_x < first_x || last_y < first_y)
        {
            throw ConfigError("mean_over() rectangle lies outside the QpMap");
        }

        double total = 0.0;
        int count = 0;
        for (int y = first_y; y <= last_y; ++y)
        {
            for (int x = first_x; x <= last_x; ++x)
            {
                total += at(x, y);
                ++count;
            }
        }
        return total / count;
    }

    // -----------------------------------------------------------------------
    // Frame
    // -----------------------------------------------------------------------

    Frame::Frame() = default;

    Frame::Frame(AVFrame* owned, const ColorSpec& color) : m_frame(owned), m_color(color) {}

    Frame::~Frame()
    {
        av_frame_free(&m_frame);
    }

    Frame::Frame(const Frame& other) : m_color(other.m_color), m_time_base(other.m_time_base)
    {
        if (other.m_frame != nullptr)
        {
            // A new reference to the same buffers, not a copy of the samples.
            m_frame = detail::ref_frame(other.m_frame).release();
        }
    }

    Frame::Frame(Frame&& other) noexcept
        : m_frame(other.m_frame), m_color(other.m_color), m_time_base(other.m_time_base)
    {
        other.m_frame = nullptr;
    }

    Frame& Frame::operator=(const Frame& other)
    {
        if (this != &other)
        {
            Frame copy(other);
            *this = std::move(copy);
        }
        return *this;
    }

    Frame& Frame::operator=(Frame&& other) noexcept
    {
        if (this != &other)
        {
            av_frame_free(&m_frame);
            m_frame = other.m_frame;
            m_color = other.m_color;
            m_time_base = other.m_time_base;
            other.m_frame = nullptr;
        }
        return *this;
    }

    Frame Frame::allocate(const int width, const int height, const PixelFormat pixel_format,
                          const ColorSpec& color, const int align)
    {
        if (width <= 0 || height <= 0)
        {
            throw ConfigError("frame dimensions must be positive, got " + std::to_string(width) +
                              "x" + std::to_string(height));
        }
        if (!pixel_format.is_valid())
        {
            throw ConfigError("Frame::allocate() requires a valid pixel format");
        }

        detail::FramePtr frame = detail::make_frame();
        frame->width = width;
        frame->height = height;
        frame->format = pixel_format.raw();

        LL_FF_CHECK(av_frame_get_buffer(frame.get(), align));

        Frame result(frame.release(), color);
        result.sync_color_to_av_frame();
        return result;
    }

    Frame Frame::from_av_frame(const AVFrame* raw, const ColorSpec& color)
    {
        if (raw == nullptr)
        {
            throw ConfigError("Frame::from_av_frame() received a null frame");
        }
        return Frame(detail::ref_frame(raw).release(), color);
    }

    Frame Frame::from_av_frame(const AVFrame* raw)
    {
        if (raw == nullptr)
        {
            throw ConfigError("Frame::from_av_frame() received a null frame");
        }
        return Frame(detail::ref_frame(raw).release(), color_from_av_frame(*raw));
    }

    bool Frame::empty() const noexcept
    {
        return m_frame == nullptr || m_frame->width <= 0 || m_frame->height <= 0;
    }

    int Frame::width() const noexcept
    {
        return m_frame != nullptr ? m_frame->width : 0;
    }

    int Frame::height() const noexcept
    {
        return m_frame != nullptr ? m_frame->height : 0;
    }

    PixelFormat Frame::pixel_format() const noexcept
    {
        return m_frame != nullptr ? PixelFormat::from_raw(m_frame->format) : PixelFormat();
    }

    std::int64_t Frame::pts() const noexcept
    {
        return m_frame != nullptr ? m_frame->pts : AV_NOPTS_VALUE;
    }

    void Frame::set_pts(const std::int64_t pts) noexcept
    {
        if (m_frame != nullptr)
        {
            m_frame->pts = pts;
        }
    }

    std::optional<double> Frame::timestamp_seconds() const noexcept
    {
        if (m_frame == nullptr || m_frame->pts == AV_NOPTS_VALUE || !m_time_base.is_valid() ||
            m_time_base.num == 0)
        {
            return std::nullopt;
        }
        return static_cast<double>(m_frame->pts) * m_time_base.to_double();
    }

    PictureType Frame::picture_type() const noexcept
    {
        return m_frame != nullptr ? from_av_picture_type(m_frame->pict_type) : PictureType::Unknown;
    }

    bool Frame::is_key_frame() const noexcept
    {
        return m_frame != nullptr && (m_frame->flags & AV_FRAME_FLAG_KEY) != 0;
    }

    int Frame::plane_count() const noexcept
    {
        return m_frame != nullptr ? pixel_format().plane_count() : 0;
    }

    ConstPlaneView Frame::plane(const int index) const
    {
        if (m_frame == nullptr)
        {
            throw ConfigError("plane() on an empty Frame");
        }
        const int count = plane_count();
        if (index < 0 || index >= count)
        {
            throw ConfigError("plane index " + std::to_string(index) + " is outside the " +
                              std::to_string(count) + " planes of " + pixel_format().name());
        }

        const PixelFormat format = pixel_format();
        const AVPixFmtDescriptor* descriptor =
            av_pix_fmt_desc_get(static_cast<AVPixelFormat>(format.raw()));

        ConstPlaneView view;
        view.data = m_frame->data[index];
        view.stride = m_frame->linesize[index];
        view.bytes_per_sample = (format.bit_depth() + 7) / 8;

        // Chroma planes are subsampled; plane 0 and any alpha plane are not.
        const bool is_chroma = index == 1 || index == 2;
        view.width = is_chroma ? AV_CEIL_RSHIFT(m_frame->width, descriptor->log2_chroma_w)
                               : m_frame->width;
        view.height = is_chroma ? AV_CEIL_RSHIFT(m_frame->height, descriptor->log2_chroma_h)
                                : m_frame->height;

        // For a packed format every component shares one plane, so a row holds
        // width * components samples rather than width.
        view.components_per_pixel =
            format.is_planar() ? 1 : static_cast<int>(descriptor->nb_components);

        return view;
    }

    PlaneView Frame::plane(const int index)
    {
        if (!is_writable())
        {
            throw ConfigError(
                "plane() requested write access to a Frame whose buffers are shared; "
                "call make_writable() or clone() first");
        }

        const ConstPlaneView view = std::as_const(*this).plane(index);
        PlaneView writable;
        writable.data = const_cast<std::uint8_t*>(view.data);
        writable.stride = view.stride;
        writable.width = view.width;
        writable.height = view.height;
        writable.bytes_per_sample = view.bytes_per_sample;
        writable.components_per_pixel = view.components_per_pixel;
        return writable;
    }

    bool Frame::is_writable() const noexcept
    {
        return m_frame != nullptr && av_frame_is_writable(m_frame) != 0;
    }

    void Frame::make_writable()
    {
        if (m_frame == nullptr)
        {
            throw ConfigError("make_writable() on an empty Frame");
        }
        LL_FF_CHECK(av_frame_make_writable(m_frame));
    }

    Frame Frame::clone() const
    {
        if (m_frame == nullptr)
        {
            return {};
        }
        Frame copy(detail::clone_frame(m_frame).release(), m_color);
        copy.m_time_base = m_time_base;
        copy.make_writable();
        return copy;
    }

    FormatDescription Frame::describe() const
    {
        FormatDescription description;
        description.width = width();
        description.height = height();
        description.pixel_format = pixel_format();
        description.color = m_color;
        return description;
    }

    void Frame::sync_color_to_av_frame()
    {
        if (m_frame == nullptr)
        {
            return;
        }
        m_frame->colorspace = static_cast<AVColorSpace>(m_color.matrix);
        m_frame->color_range = static_cast<AVColorRange>(m_color.range);
        m_frame->color_primaries = static_cast<AVColorPrimaries>(m_color.primaries);
        m_frame->color_trc = static_cast<AVColorTransferCharacteristic>(m_color.transfer);
        m_frame->chroma_location = static_cast<AVChromaLocation>(m_color.chroma_location);
    }

    std::optional<QpMap> Frame::qp_map() const
    {
        if (m_frame == nullptr)
        {
            return std::nullopt;
        }

        const AVFrameSideData* side =
            av_frame_get_side_data(m_frame, AV_FRAME_DATA_VIDEO_ENC_PARAMS);
        if (side == nullptr)
        {
            return std::nullopt;
        }

        const auto* params = reinterpret_cast<const AVVideoEncParams*>(side->data);
        if (params->nb_blocks == 0)
        {
            // Frame-level quantization only: one block covering everything,
            // which keeps the accessor's shape the same either way.
            QpMap map;
            map.block_width = m_frame->width;
            map.block_height = m_frame->height;
            map.width = 1;
            map.height = 1;
            map.values.push_back(static_cast<int>(params->qp));
            return map;
        }

        // Block geometry is per-block in FFmpeg's representation, so the grid
        // size is recovered from the first block and the frame extent.
        const AVVideoBlockParams* first = av_video_enc_params_block(
            const_cast<AVVideoEncParams*>(params), 0);
        QpMap map;
        map.block_width = first->w > 0 ? first->w : 16;
        map.block_height = first->h > 0 ? first->h : 16;
        map.width = (m_frame->width + map.block_width - 1) / map.block_width;
        map.height = (m_frame->height + map.block_height - 1) / map.block_height;
        map.values.assign(static_cast<std::size_t>(map.width) *
                              static_cast<std::size_t>(map.height),
                          static_cast<int>(params->qp));

        for (unsigned i = 0; i < params->nb_blocks; ++i)
        {
            const AVVideoBlockParams* block =
                av_video_enc_params_block(const_cast<AVVideoEncParams*>(params), i);
            const int block_x = block->src_x / map.block_width;
            const int block_y = block->src_y / map.block_height;
            if (block_x < 0 || block_x >= map.width || block_y < 0 || block_y >= map.height)
            {
                continue;
            }
            map.values[static_cast<std::size_t>(block_y) * static_cast<std::size_t>(map.width) +
                       static_cast<std::size_t>(block_x)] =
                static_cast<int>(params->qp + block->delta_qp);
        }

        return map;
    }
}
