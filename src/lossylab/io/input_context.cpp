#include "lossylab/io/input_context.hpp"

#include "lossylab/core/error.hpp"
#include "lossylab/detail/ff_error.hpp"

#include <algorithm>

namespace lossylab::detail
{
    namespace
    {
        /// Size of the buffer FFmpeg reads through. 32 KiB is FFmpeg's own
        /// default and large enough that probing a container header rarely
        /// needs a second read.
        constexpr int io_buffer_size = 32768;
    }

    int InputContext::read_packet(void* opaque, std::uint8_t* buffer, const int size)
    {
        auto* cursor = static_cast<MemoryCursor*>(opaque);
        const auto remaining =
            static_cast<std::int64_t>(cursor->data.size()) - cursor->position;
        if (remaining <= 0)
        {
            return AVERROR_EOF;
        }

        const int count = static_cast<int>(std::min<std::int64_t>(size, remaining));
        std::copy_n(cursor->data.data() + cursor->position, count, buffer);
        cursor->position += count;
        return count;
    }

    std::int64_t InputContext::seek(void* opaque, const std::int64_t offset, const int whence)
    {
        auto* cursor = static_cast<MemoryCursor*>(opaque);
        const auto size = static_cast<std::int64_t>(cursor->data.size());

        // FFmpeg asks for the stream length through this flag rather than a
        // separate callback.
        if ((whence & AVSEEK_SIZE) != 0)
        {
            return size;
        }

        std::int64_t target = 0;
        switch (whence & ~AVSEEK_FORCE)
        {
        case SEEK_SET: target = offset; break;
        case SEEK_CUR: target = cursor->position + offset; break;
        case SEEK_END: target = size + offset; break;
        default: return AVERROR(EINVAL);
        }

        if (target < 0 || target > size)
        {
            return AVERROR(EINVAL);
        }
        cursor->position = target;
        return target;
    }

    InputContext::InputContext(const Source& source)
    {
        AVFormatContext* format = nullptr;

        if (source.is_path())
        {
            const int status =
                avformat_open_input(&format, source.path().c_str(), nullptr, nullptr);
            if (status < 0)
            {
                throw FFmpegError(status, "avformat_open_input",
                                  averror_string(status) + " opening " + source.describe());
            }
            m_format.reset(format);
            return;
        }

        if (source.bytes().empty())
        {
            throw ConfigError("cannot open an empty memory source");
        }

        m_cursor = std::make_unique<MemoryCursor>(MemoryCursor{source.bytes(), 0});

        // avio takes ownership of this buffer and may reallocate it, so it is
        // released to the raw pointer and reclaimed by AvIoContextDeleter.
        AvBufferPtr buffer(LL_FF_ALLOC(av_malloc(io_buffer_size)));
        m_io_context.reset(LL_FF_ALLOC(avio_alloc_context(
            static_cast<std::uint8_t*>(buffer.release()), io_buffer_size, 0, m_cursor.get(),
            &InputContext::read_packet, nullptr, &InputContext::seek)));

        format = LL_FF_ALLOC(avformat_alloc_context());
        format->pb = m_io_context.get();

        // Tells FFmpeg the input is not seekable through a URL, so it uses the
        // callbacks above rather than trying to reopen by name.
        format->flags |= AVFMT_FLAG_CUSTOM_IO;

        const int status = avformat_open_input(&format, nullptr, nullptr, nullptr);
        if (status < 0)
        {
            // On failure avformat_open_input has already freed the context.
            throw FFmpegError(status, "avformat_open_input",
                              averror_string(status) + " reading " + source.describe());
        }
        m_format.reset(format);
    }

    void InputContext::find_stream_info()
    {
        LL_FF_CHECK(avformat_find_stream_info(m_format.get(), nullptr));
    }
}
