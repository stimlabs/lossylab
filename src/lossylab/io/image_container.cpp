#include "lossylab/io/image_container.hpp"

#include "lossylab/io/source_reader.hpp"

#include <zlib.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <span>
#include <string_view>
#include <vector>

namespace lossylab::detail
{
    namespace
    {
        std::uint32_t little_endian_24(const std::uint8_t* bytes)
        {
            return static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8) |
                   (static_cast<std::uint32_t>(bytes[2]) << 16);
        }

        std::uint32_t little_endian_32(const std::uint8_t* bytes)
        {
            return little_endian_24(bytes) | (static_cast<std::uint32_t>(bytes[3]) << 24);
        }

        std::uint32_t big_endian_32(const std::uint8_t* bytes)
        {
            return (static_cast<std::uint32_t>(bytes[0]) << 24) | (static_cast<std::uint32_t>(bytes[1]) << 16) |
                   (static_cast<std::uint32_t>(bytes[2]) << 8) | static_cast<std::uint32_t>(bytes[3]);
        }

        /// Inflates a zlib stream, stopping at `embedded_payload_limit`
        /// bytes of output so a small chunk cannot expand without bound.
        /// Nullopt when the stream is damaged or would exceed the limit.
        std::optional<std::vector<std::uint8_t>> inflate_bounded(const std::span<const std::uint8_t> compressed)
        {
            z_stream stream{};
            if (inflateInit(&stream) != Z_OK)
            {
                return std::nullopt;
            }
            stream.next_in = const_cast<Bytef*>(compressed.data());
            stream.avail_in = static_cast<uInt>(compressed.size());

            std::vector<std::uint8_t> output;
            constexpr std::size_t step = 64 * 1024;
            int status = Z_OK;
            while (status == Z_OK && output.size() < static_cast<std::size_t>(embedded_payload_limit))
            {
                const std::size_t written = output.size();
                output.resize(written + step);
                stream.next_out = output.data() + written;
                stream.avail_out = static_cast<uInt>(step);
                status = inflate(&stream, Z_NO_FLUSH);
                output.resize(written + step - stream.avail_out);
            }
            inflateEnd(&stream);
            if (status != Z_STREAM_END)
            {
                return std::nullopt;
            }
            return output;
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
                else if ((fourcc_is(header, 0, "ICCP") && !chunks.icc_profile.has_value()) ||
                         (fourcc_is(header, 0, "EXIF") && !chunks.exif.has_value()))
                {
                    const bool is_icc = fourcc_is(header, 0, "ICCP");
                    std::vector<std::uint8_t> payload;
                    if (payload_size > embedded_payload_limit)
                    {
                        if (is_icc)
                        {
                            chunks.icc_problems.push_back("the ICCP chunk's " + std::to_string(payload_size) +
                                                          " bytes exceed the " +
                                                          std::to_string(embedded_payload_limit) +
                                                          "-byte limit and were not read");
                        }
                    }
                    else
                    {
                        payload = reader.read(payload_offset, static_cast<std::size_t>(payload_size));
                        if (is_icc && payload.size() < static_cast<std::size_t>(payload_size))
                        {
                            chunks.icc_problems.emplace_back("the ICCP chunk is cut short");
                        }
                    }
                    (is_icc ? chunks.icc_profile : chunks.exif) = std::move(payload);
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

    std::optional<PngChunks> read_png_chunks(const Source& source)
    {
        constexpr std::array<std::uint8_t, 8> png_signature = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
        SourceReader reader(source);
        const std::vector<std::uint8_t> signature = reader.read(0, png_signature.size());
        if (!std::equal(signature.begin(), signature.end(), png_signature.begin(), png_signature.end()))
        {
            return std::nullopt;
        }

        // Each chunk is a 4-byte length, a 4-byte type, the data and a CRC.
        PngChunks chunks;
        constexpr std::int64_t chunk_header_size = 8;
        constexpr std::int64_t crc_size = 4;
        std::int64_t offset = png_signature.size();
        while (true)
        {
            const std::vector<std::uint8_t> header = reader.read(offset, chunk_header_size);
            if (header.size() < chunk_header_size || fourcc_is(header, 4, "IEND"))
            {
                break;
            }
            const std::int64_t payload_offset = offset + chunk_header_size;
            const std::int64_t payload_size = big_endian_32(header.data());
            const bool is_icc = fourcc_is(header, 4, "iCCP") && !chunks.icc_profile.has_value();
            const bool is_exif = fourcc_is(header, 4, "eXIf") && !chunks.exif.has_value();
            if (is_icc && payload_size > embedded_payload_limit)
            {
                chunks.icc_problems.push_back("the iCCP chunk's " + std::to_string(payload_size) +
                                              " bytes exceed the " + std::to_string(embedded_payload_limit) +
                                              "-byte limit and were not read");
                chunks.icc_profile = std::vector<std::uint8_t>();
            }
            else if (is_exif && payload_size > embedded_payload_limit)
            {
                chunks.exif = std::vector<std::uint8_t>();
            }
            else if (is_exif)
            {
                chunks.exif = reader.read(payload_offset, static_cast<std::size_t>(payload_size));
            }
            else if (is_icc)
            {
                // A Latin-1 name of 1-79 bytes, a NUL, a compression method
                // (0, zlib), then the compressed profile.
                const std::vector<std::uint8_t> payload =
                    reader.read(payload_offset, static_cast<std::size_t>(payload_size));
                const auto terminator = std::find(payload.begin(), payload.end(), std::uint8_t{0});
                if (terminator == payload.end() || terminator + 1 == payload.end() || *(terminator + 1) != 0)
                {
                    chunks.icc_problems.emplace_back("the iCCP chunk has no name terminator or an unknown compression");
                    chunks.icc_profile = std::vector<std::uint8_t>();
                }
                else
                {
                    chunks.icc_profile_name.assign(payload.begin(), terminator);
                    const auto compressed_start = static_cast<std::size_t>(terminator - payload.begin()) + 2;
                    std::optional<std::vector<std::uint8_t>> profile =
                        inflate_bounded(std::span<const std::uint8_t>(payload).subspan(compressed_start));
                    if (!profile.has_value())
                    {
                        chunks.icc_problems.emplace_back("the iCCP chunk's profile does not decompress within the limit");
                    }
                    chunks.icc_profile = std::move(profile).value_or(std::vector<std::uint8_t>());
                }
            }
            offset = payload_offset + payload_size + crc_size;
        }
        return chunks;
    }
}
