#include "lossylab/io/source.hpp"

#include "lossylab/core/error.hpp"
#include "lossylab/detail/ff_ptr.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <utility>

namespace lossylab
{
    namespace
    {
        std::string lowercase(std::string text)
        {
            std::transform(text.begin(), text.end(), text.begin(),
                           [](const unsigned char c) { return std::tolower(c); });
            return text;
        }
    }

    Source Source::from_path(std::string path)
    {
        if (path.empty())
        {
            throw ConfigError("Source::from_path() received an empty path");
        }
        Source source;
        source.m_path = std::move(path);
        return source;
    }

    Source Source::from_memory(const std::span<const std::uint8_t> bytes,
                               std::string extension_hint)
    {
        Source source;
        source.m_borrowed_bytes = bytes;
        source.m_extension_hint = lowercase(std::move(extension_hint));
        return source;
    }

    Source Source::from_bytes(std::vector<std::uint8_t> bytes, std::string extension_hint)
    {
        Source source;
        source.m_owned_bytes = std::move(bytes);
        source.m_extension_hint = lowercase(std::move(extension_hint));
        return source;
    }

    std::span<const std::uint8_t> Source::bytes() const noexcept
    {
        // Spanned on each call rather than stored, so a copied Source reads its
        // own buffer instead of the one it was copied from.
        if (!m_owned_bytes.empty())
        {
            return m_owned_bytes;
        }
        return m_borrowed_bytes;
    }

    std::string Source::claimed_extension() const
    {
        if (!is_path())
        {
            return m_extension_hint;
        }

        const std::size_t slash = m_path.find_last_of("/\\");
        const std::size_t dot = m_path.find_last_of('.');
        if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
        {
            return {};
        }
        return lowercase(m_path.substr(dot + 1));
    }

    std::string Source::describe() const
    {
        if (is_path())
        {
            return m_path;
        }
        return "<" + std::to_string(bytes().size()) + " bytes in memory>";
    }

    std::string Source::sha256() const
    {
        std::call_once(m_hash_cache->once, [this] { m_hash_cache->value = compute_sha256(); });
        return m_hash_cache->value;
    }

    std::string Source::compute_sha256() const
    {
        if (!is_path())
        {
            return detail::sha256_hex(bytes());
        }

        std::ifstream file(m_path, std::ios::binary);
        if (!file)
        {
            throw ConfigError("cannot open " + m_path + " to hash it");
        }

        detail::Sha256Stream stream;
        std::array<char, 1 << 16> buffer{};
        for (;;)
        {
            file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const auto bytes_read = static_cast<std::size_t>(file.gcount());
            if (bytes_read > 0)
            {
                stream.feed(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(buffer.data()),
                                                           bytes_read));
            }
            if (!file)
            {
                break;
            }
        }
        return stream.finish();
    }
}
