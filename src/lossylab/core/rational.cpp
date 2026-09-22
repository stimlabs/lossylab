#include "lossylab/core/rational.hpp"

#include "lossylab/core/error.hpp"

#include <charconv>
#include <cstdlib>
#include <numeric>

namespace lossylab
{
    double Rational::to_double() const noexcept
    {
        return den == 0 ? 0.0 : static_cast<double>(num) / static_cast<double>(den);
    }

    Rational Rational::reduced() const noexcept
    {
        if (den == 0)
        {
            return *this;
        }
        const int divisor = std::gcd(num, den);
        if (divisor == 0)
        {
            return *this;
        }
        Rational result{num / divisor, den / divisor};
        if (result.den < 0)
        {
            // Keep the sign on the numerator, so equal values compare equal.
            result.num = -result.num;
            result.den = -result.den;
        }
        return result;
    }

    std::string Rational::to_string() const
    {
        return std::to_string(num) + '/' + std::to_string(den);
    }

    Rational Rational::parse(const std::string_view text)
    {
        const std::size_t slash = text.find('/');

        const auto to_int = [&text](const std::string_view part) {
            int value = 0;
            const auto* end = part.data() + part.size();
            const auto [ptr, ec] = std::from_chars(part.data(), end, value);
            if (ec != std::errc{} || ptr != end)
            {
                throw ConfigError("invalid rational '" + std::string(text) + "'");
            }
            return value;
        };

        if (slash == std::string_view::npos)
        {
            return {to_int(text), 1};
        }
        return {to_int(text.substr(0, slash)), to_int(text.substr(slash + 1))};
    }

    json::Value Rational::to_json() const
    {
        return json::Value(to_string());
    }

    Rational Rational::from_json(const json::Value& value)
    {
        return parse(value.get_ref<const std::string&>());
    }

    bool operator==(const Rational left, const Rational right) noexcept
    {
        // Compared after reduction, so 1/2 and 2/4 are the same rate.
        const Rational reduced_left = left.reduced();
        const Rational reduced_right = right.reduced();
        return reduced_left.num == reduced_right.num && reduced_left.den == reduced_right.den;
    }
}
