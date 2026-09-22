#pragma once

#include "lossylab/core/json.hpp"

#include <string>

namespace lossylab
{
    /// An exact rational, used for time bases, frame rates and aspect ratios.
    ///
    /// Kept exact rather than collapsed to a double because 30000/1001 and
    /// 29.97 are not the same thing, and the difference shows up as drift once
    /// timestamps accumulate over a clip.
    struct Rational
    {
        int num = 0;
        int den = 1;

        [[nodiscard]] double to_double() const noexcept;
        [[nodiscard]] bool is_valid() const noexcept { return den != 0; }

        /// Reduced to lowest terms.
        [[nodiscard]] Rational reduced() const noexcept;

        [[nodiscard]] Rational inverse() const noexcept { return {den, num}; }

        /// "30000/1001".
        [[nodiscard]] std::string to_string() const;

        /// Parses "num/den" or a bare integer.
        static Rational parse(std::string_view text);

        [[nodiscard]] json::Value to_json() const;
        static Rational from_json(const json::Value& value);
    };

    bool operator==(Rational left, Rational right) noexcept;
    inline bool operator!=(const Rational left, const Rational right) noexcept
    {
        return !(left == right);
    }
}
