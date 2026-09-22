#include "lossylab/io/image_container.hpp"

#include "lossylab/core/error.hpp"
#include "lossylab/detail/ff_error.hpp"
#include "lossylab/detail/ff_ptr.hpp"

#include <algorithm>
#include <cstring>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace lossylab::detail
{
    namespace
    {
        struct OpenedIoContextDeleter
        {
            void operator()(AVIOContext* pointer) const noexcept { avio_closep(&pointer); }
        };

        /// Reads a Source's bytes at arbitrary offsets without a demuxer: from
        /// the buffer for a memory source, through FFmpeg's I/O layer for a
        /// path, so URLs work wherever probe() accepts them.
        class SourceReader
        {
        public:
            explicit SourceReader(const Source& source)
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

            /// Up to `count` bytes starting at `offset`; fewer at the end of the
            /// input.
            std::vector<std::uint8_t> read(const std::int64_t offset, const std::size_t count)
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

        private:
            std::span<const std::uint8_t> m_bytes;
            std::unique_ptr<AVIOContext, OpenedIoContextDeleter> m_io;
        };

        std::uint32_t little_endian_24(const std::uint8_t* bytes)
        {
            return static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8) |
                   (static_cast<std::uint32_t>(bytes[2]) << 16);
        }

        std::uint32_t little_endian_32(const std::uint8_t* bytes)
        {
            return little_endian_24(bytes) | (static_cast<std::uint32_t>(bytes[3]) << 24);
        }

        bool fourcc_is(const std::vector<std::uint8_t>& bytes, const std::size_t offset, const std::string_view fourcc)
        {
            return bytes.size() >= offset + 4 && std::memcmp(bytes.data() + offset, fourcc.data(), 4) == 0;
        }

        /// Walks WebP chunks from `offset` to `end`, recording what the image
        /// data chunks say. ANMF frames hold chunks of their own after a
        /// 16-byte frame header. They are walked into only at the top level,
        /// the one place the format allows them, so nesting depth stays at
        /// one however the file is built.
        void walk_webp_chunks(SourceReader& reader, std::int64_t offset, const std::int64_t end, WebpChunks& chunks,
                              const bool top_level)
        {
            constexpr std::int64_t chunk_header_size = 8;
            constexpr std::int64_t frame_header_size = 16;

            while (offset + chunk_header_size <= end)
            {
                const std::vector<std::uint8_t> header = reader.read(offset, chunk_header_size);
                if (header.size() < chunk_header_size)
                {
                    return;
                }
                const std::int64_t payload_offset = offset + chunk_header_size;
                const std::int64_t payload_size = little_endian_32(header.data() + 4);

                if (fourcc_is(header, 0, "VP8 "))
                {
                    chunks.has_lossy = true;
                }
                else if (fourcc_is(header, 0, "VP8L"))
                {
                    chunks.has_lossless = true;

                    // A signature byte, then width - 1 and height - 1 in 14
                    // bits each and the alpha_is_used bit.
                    const std::vector<std::uint8_t> vp8l_header = reader.read(payload_offset, 5);
                    if (vp8l_header.size() == 5 && ((little_endian_32(vp8l_header.data() + 1) >> 28) & 1) != 0)
                    {
                        chunks.has_alpha = true;
                    }
                }
                else if (fourcc_is(header, 0, "ALPH"))
                {
                    chunks.has_alpha = true;
                }
                else if (fourcc_is(header, 0, "VP8X"))
                {
                    // Flags, three reserved bytes, then canvas width - 1 and
                    // height - 1 in 24 bits each.
                    const std::vector<std::uint8_t> extended = reader.read(payload_offset, 10);
                    if (extended.size() == 10)
                    {
                        chunks.has_alpha = chunks.has_alpha || (extended[0] & 0x10) != 0;
                        chunks.is_animated = chunks.is_animated || (extended[0] & 0x02) != 0;
                        chunks.canvas_width = static_cast<int>(little_endian_24(extended.data() + 4)) + 1;
                        chunks.canvas_height = static_cast<int>(little_endian_24(extended.data() + 7)) + 1;
                    }
                }
                else if (fourcc_is(header, 0, "ANIM"))
                {
                    chunks.is_animated = true;
                }
                else if (fourcc_is(header, 0, "ANMF") && top_level)
                {
                    chunks.is_animated = true;
                    ++chunks.frame_count;
                    walk_webp_chunks(reader, payload_offset + frame_header_size, payload_offset + payload_size,
                                     chunks, false);
                }

                // Chunks are padded to an even length.
                offset = payload_offset + payload_size + (payload_size & 1);
            }
        }
    }

    std::optional<WebpChunks> read_webp_chunks(const Source& source)
    {
        SourceReader reader(source);
        const std::vector<std::uint8_t> riff_header = reader.read(0, 12);
        if (!fourcc_is(riff_header, 0, "RIFF") || !fourcc_is(riff_header, 8, "WEBP"))
        {
            return std::nullopt;
        }

        WebpChunks chunks;
        chunks.frame_count = 0;
        const std::int64_t riff_end = 8 + static_cast<std::int64_t>(little_endian_32(riff_header.data() + 4));
        walk_webp_chunks(reader, 12, riff_end, chunks, true);
        if (!chunks.is_animated)
        {
            chunks.frame_count = 1;
        }
        return chunks;
    }
}
