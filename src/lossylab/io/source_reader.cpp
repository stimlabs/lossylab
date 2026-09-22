#include "lossylab/io/source_reader.hpp"

#include "lossylab/core/error.hpp"
#include "lossylab/detail/ff_error.hpp"
#include "lossylab/detail/ff_ptr.hpp"

#include <algorithm>

namespace lossylab::detail
{
    namespace
    {
        constexpr std::size_t sequential_buffer_size = 65536;
    }

    void SourceReader::IoContextDeleter::operator()(AVIOContext* pointer) const noexcept
    {
        avio_closep(&pointer);
    }

    SourceReader::SourceReader(const Source& source)
    {
        if (!source.is_path())
        {
            m_bytes = source.bytes();
            return;
        }

        AVIOContext* opened = nullptr;
        const int status = avio_open2(&opened, source.path().c_str(), AVIO_FLAG_READ, nullptr, nullptr);
        if (status < 0)
        {
            throw FFmpegError(status, "avio_open2", averror_string(status) + " opening " + source.describe());
        }
        m_io.reset(opened);
    }

    SourceReader::~SourceReader() = default;

    std::vector<std::uint8_t> SourceReader::read(const std::int64_t offset, const std::size_t count)
    {
        if (!m_io)
        {
            if (offset < 0 || static_cast<std::size_t>(offset) >= m_bytes.size())
            {
                return {};
            }
            const std::size_t available = std::min(count, m_bytes.size() - static_cast<std::size_t>(offset));
            const auto begin = m_bytes.begin() + offset;
            return {begin, begin + static_cast<std::ptrdiff_t>(available)};
        }

        if (avio_seek(m_io.get(), offset, SEEK_SET) < 0)
        {
            return {};
        }
        std::vector<std::uint8_t> bytes(count);
        const int read = avio_read(m_io.get(), bytes.data(), static_cast<int>(count));
        bytes.resize(read > 0 ? static_cast<std::size_t>(read) : 0);
        return bytes;
    }

    std::optional<std::int64_t> SourceReader::size() const
    {
        if (!m_io)
        {
            return static_cast<std::int64_t>(m_bytes.size());
        }
        const std::int64_t size = avio_size(m_io.get());
        return size >= 0 ? std::optional<std::int64_t>(size) : std::nullopt;
    }

    bool SequentialReader::refill()
    {
        m_buffer_offset += static_cast<std::int64_t>(m_buffer.size());
        m_cursor = 0;
        m_buffer = m_reader.read(m_buffer_offset, sequential_buffer_size);
        return !m_buffer.empty();
    }

    std::optional<std::uint8_t> SequentialReader::next()
    {
        if (m_cursor >= static_cast<std::int64_t>(m_buffer.size()) && !refill())
        {
            return std::nullopt;
        }
        return m_buffer[static_cast<std::size_t>(m_cursor++)];
    }

    std::vector<std::uint8_t> SequentialReader::take(const std::size_t count)
    {
        std::vector<std::uint8_t> bytes;
        bytes.reserve(count);
        while (bytes.size() < count)
        {
            if (m_cursor >= static_cast<std::int64_t>(m_buffer.size()) && !refill())
            {
                break;
            }
            const std::size_t available =
                std::min(count - bytes.size(), m_buffer.size() - static_cast<std::size_t>(m_cursor));
            const auto begin = m_buffer.begin() + m_cursor;
            bytes.insert(bytes.end(), begin, begin + static_cast<std::ptrdiff_t>(available));
            m_cursor += static_cast<std::int64_t>(available);
        }
        return bytes;
    }

    void SequentialReader::skip(const std::int64_t count)
    {
        const std::int64_t target = std::max<std::int64_t>(0, position() + count);
        if (target >= m_buffer_offset && target < m_buffer_offset + static_cast<std::int64_t>(m_buffer.size()))
        {
            m_cursor = target - m_buffer_offset;
            return;
        }
        m_buffer.clear();
        m_buffer_offset = target;
        m_cursor = 0;
    }
}
