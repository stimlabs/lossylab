#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
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
        ///
        /// `extension_hint` records what the caller believes the format to
        /// be, such as the extension of the file the bytes came from, with
        /// no effect on how the bytes are decoded: decoding always goes by
        /// sniffing the bytes themselves. `probe()` compares the sniffed
        /// format against this hint and reports a mismatch, since a buffer
        /// whose declared format disagrees with its content is evidence the
        /// file was converted or mislabeled somewhere upstream.
        static Source from_memory(std::span<const std::uint8_t> bytes,
                                  std::string extension_hint = {});

        /// Takes ownership of a buffer. See `from_memory` for `extension_hint`.
        static Source from_bytes(std::vector<std::uint8_t> bytes,
                                 std::string extension_hint = {});

        [[nodiscard]] bool is_path() const noexcept { return !m_path.empty(); }
        [[nodiscard]] const std::string& path() const noexcept { return m_path; }
        [[nodiscard]] std::span<const std::uint8_t> bytes() const noexcept;

        /// The extension this source claims, lowercased and without the
        /// leading dot: the path's suffix for a path source, or
        /// `extension_hint` for a memory source. Empty when neither is
        /// available.
        [[nodiscard]] std::string claimed_extension() const;

        /// A short label for error messages: the path, or a note of the
        /// buffer's size. Never the buffer contents, and never stored in a
        /// record: a record identifies this source by `sha256()`, not by
        /// this label, so that replay does not depend on where the bytes
        /// happened to live.
        [[nodiscard]] std::string describe() const;

        /// "sha256:" and the lowercase hex SHA-256 of this source's bytes: the
        /// file's contents for a path source, or the buffer itself for a
        /// memory source. Computed once and cached, since this runs once per
        /// file in an audit and per sample in a dataloader; safe to call from
        /// several threads on the same Source.
        [[nodiscard]] std::string sha256() const;

    private:
        [[nodiscard]] std::string compute_sha256() const;

        std::string m_path;
        std::string m_extension_hint;
        std::vector<std::uint8_t> m_owned_bytes;
        std::span<const std::uint8_t> m_borrowed_bytes;

        struct HashCache
        {
            std::once_flag once;
            std::string value;
        };

        /// Behind a shared_ptr, rather than a direct member, so Source stays
        /// movable and copyable despite std::once_flag being neither: moving
        /// or copying a Source carries the cache along with its bytes.
        mutable std::shared_ptr<HashCache> m_hash_cache = std::make_shared<HashCache>();
    };
}
