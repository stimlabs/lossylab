#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace lossylab
{
    /// Where bytes come from: a file, or a buffer already in memory.
    ///
    /// Memory sources are the common case for a dataloader, which usually has
    /// the bytes in hand and should not pay for a temporary file to hand them
    /// to FFmpeg.
    class Source
    {
    public:
        /// Reads from a path. FFmpeg's protocols apply, so this also accepts
        /// URLs when the build supports them.
        static Source from_path(std::string path);

        /// Borrows a buffer. The caller keeps ownership, and the data must
        /// outlive every operation that reads from this Source.
        static Source from_memory(std::span<const std::uint8_t> bytes);

        /// Takes ownership of a buffer.
        static Source from_bytes(std::vector<std::uint8_t> bytes);

        [[nodiscard]] bool is_path() const noexcept { return !m_path.empty(); }
        [[nodiscard]] const std::string& path() const noexcept { return m_path; }
        [[nodiscard]] std::span<const std::uint8_t> bytes() const noexcept;

        /// A short label for error messages and records: the path, or a note of
        /// the buffer's size. Never the buffer contents.
        [[nodiscard]] std::string describe() const;

    private:
        std::string m_path;
        std::vector<std::uint8_t> m_owned_bytes;
        std::span<const std::uint8_t> m_borrowed_bytes;
    };
}
