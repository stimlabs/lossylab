#include "lossylab/io/source.hpp"

#include "lossylab/core/error.hpp"

#include <algorithm>
#include <cctype>
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
}
