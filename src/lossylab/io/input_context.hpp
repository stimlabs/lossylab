#pragma once

/// Opening a Source as an AVFormatContext. Internal header.

#include "lossylab/detail/ff_ptr.hpp"
#include "lossylab/io/source.hpp"

#include <cstdint>
#include <span>

namespace lossylab::detail
{
    /// An open input, whether backed by a file or by a buffer.
    ///
    /// For a memory source the AVIOContext and its buffer have to outlive the
    /// AVFormatContext that reads through them, and FFmpeg does not manage that
    /// for us. Owning all three together in one object is what keeps the
    /// ordering correct no matter how the caller unwinds.
    class InputContext
    {
    public:
        explicit InputContext(const Source& source);

        InputContext(const InputContext&) = delete;
        InputContext& operator=(const InputContext&) = delete;
        InputContext(InputContext&&) noexcept = default;
        InputContext& operator=(InputContext&&) noexcept = default;
        ~InputContext() = default;

        [[nodiscard]] AVFormatContext* get() noexcept { return m_format.get(); }
        [[nodiscard]] const AVFormatContext* get() const noexcept { return m_format.get(); }

        AVFormatContext* operator->() noexcept { return m_format.get(); }

        /// Reads stream properties, decoding a few packets if the container
        /// does not declare them outright.
        void find_stream_info();

    private:
        /// Reader state for a memory source: the buffer and the cursor into it.
        struct MemoryCursor
        {
            std::span<const std::uint8_t> data;
            std::int64_t position = 0;
        };

        static int read_packet(void* opaque, std::uint8_t* buffer, int size);
        static std::int64_t seek(void* opaque, std::int64_t offset, int whence);

        // Declaration order is destruction order reversed, so the format
        // context is torn down before the AVIO context it reads through.
        std::unique_ptr<MemoryCursor> m_cursor;
        AvIoContextPtr m_io_context;
        FormatContextPtr m_format;
    };
}
