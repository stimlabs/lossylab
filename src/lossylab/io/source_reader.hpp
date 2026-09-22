#pragma once

/// Reading a Source's raw bytes without a demuxer. Internal header.

#include "lossylab/io/source.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

struct AVIOContext;

namespace lossylab::detail
{
    /// Reads a Source's bytes at arbitrary offsets: from the buffer for a
    /// memory source, through FFmpeg's I/O layer for a path, so URLs work
    /// wherever probe() accepts them.
    class SourceReader
    {
    public:
        explicit SourceReader(const Source& source);
        ~SourceReader();

        SourceReader(const SourceReader&) = delete;
        SourceReader& operator=(const SourceReader&) = delete;

        /// Up to `count` bytes starting at `offset`; fewer at the end of the
        /// input.
        [[nodiscard]] std::vector<std::uint8_t> read(std::int64_t offset, std::size_t count);

        /// The input's size, when it is known.
        [[nodiscard]] std::optional<std::int64_t> size() const;

    private:
        struct IoContextDeleter
        {
            void operator()(AVIOContext* pointer) const noexcept;
        };

        std::span<const std::uint8_t> m_bytes;
        std::unique_ptr<AVIOContext, IoContextDeleter> m_io;
    };

    /// Reads a Source front to back through a buffer, for formats walked
    /// byte by byte.
    class SequentialReader
    {
    public:
        explicit SequentialReader(SourceReader& reader) : m_reader(reader) {}

        /// The next byte, or nullopt at the end of the input.
        [[nodiscard]] std::optional<std::uint8_t> next();

        /// The next `count` bytes; fewer at the end of the input.
        [[nodiscard]] std::vector<std::uint8_t> take(std::size_t count);

        void skip(std::int64_t count);

        /// Offset of the next byte `next()` returns.
        [[nodiscard]] std::int64_t position() const noexcept { return m_buffer_offset + m_cursor; }

    private:
        /// Refills the buffer at the current position. False at the end.
        bool refill();

        SourceReader& m_reader;
        std::vector<std::uint8_t> m_buffer;
        std::int64_t m_buffer_offset = 0;
        std::int64_t m_cursor = 0;
    };
}
