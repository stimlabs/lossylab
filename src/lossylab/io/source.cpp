#include "lossylab/io/source.hpp"

#include "lossylab/core/error.hpp"

#include <utility>

namespace lossylab
{
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

    Source Source::from_memory(const std::span<const std::uint8_t> bytes)
    {
        Source source;
        source.m_borrowed_bytes = bytes;
        return source;
    }

    Source Source::from_bytes(std::vector<std::uint8_t> bytes)
    {
        Source source;
        source.m_owned_bytes = std::move(bytes);
        source.m_borrowed_bytes = std::span<const std::uint8_t>(source.m_owned_bytes);
        return source;
    }

    std::span<const std::uint8_t> Source::bytes() const noexcept
    {
        return m_borrowed_bytes;
    }

    std::string Source::describe() const
    {
        if (is_path())
        {
            return m_path;
        }
        return "<" + std::to_string(m_borrowed_bytes.size()) + " bytes in memory>";
    }
}
