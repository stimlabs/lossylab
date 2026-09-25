#pragma once

#include "lossylab/core/reflect.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <numeric>
#include <ranges>
#include <type_traits>
#include <vector>

namespace lossylab
{
    /// Statistics over a range of numbers, returned as double.
    ///
    /// The standard library has no mean, standard deviation or median, so these
    /// are built from its algorithms (std::nth_element, std::midpoint) and a
    /// pairwise sum, as numpy does: the rounding error of a sum of n values grows
    /// with log n instead of n. Integers are summed as doubles. Like numpy, NaN
    /// and infinities propagate through every result, but an undefined result
    /// (no values, or no degrees of freedom) is NaN where numpy divides by zero
    /// and can return infinity.
    namespace statistics
    {
        template <class Range>
        concept number_range = std::ranges::random_access_range<Range> && std::ranges::sized_range<Range> &&
                               std::is_arithmetic_v<std::ranges::range_value_t<Range>>;

        namespace detail
        {
            constexpr double not_a_number = std::numeric_limits<double>::quiet_NaN();

            /// Below this many values a plain loop is summed directly.
            constexpr std::size_t pairwise_block = 128;

            /// The sum of term(first[0]) ... term(first[count - 1]), by halving
            /// down to blocks of `pairwise_block`.
            template <class Iterator, class Term>
            double pairwise_sum(const Iterator first, const std::size_t count, const Term& term)
            {
                if (count <= pairwise_block)
                {
                    double sum = 0.0;
                    for (std::size_t i = 0; i < count; ++i)
                    {
                        sum += term(first[static_cast<std::ptrdiff_t>(i)]);
                    }
                    return sum;
                }
                const std::size_t half = count / 2;
                return pairwise_sum(first, half, term) +
                       pairwise_sum(first + static_cast<std::ptrdiff_t>(half), count - half, term);
            }

            template <number_range Range>
            double sum_of(const Range& values)
            {
                return pairwise_sum(std::ranges::begin(values), std::ranges::size(values),
                                    [](const auto value) { return static_cast<double>(value); });
            }

            /// NaN when there are no degrees of freedom left.
            template <number_range Range>
            double variance_given_mean(const Range& values, const double center, const std::size_t ddof)
            {
                const std::size_t count = std::ranges::size(values);
                if (count <= ddof)
                {
                    return not_a_number;
                }
                const double squared_deviations =
                    pairwise_sum(std::ranges::begin(values), count, [center](const auto value) {
                        const double deviation = static_cast<double>(value) - center;
                        return deviation * deviation;
                    });
                return squared_deviations / static_cast<double>(count - ddof);
            }

            /// Partially orders a copy with nth_element, which is all a median
            /// needs, so the input's order is kept. NaN when empty or when a
            /// value is NaN, which has no order.
            template <number_range Range>
            double median_of(const Range& values)
            {
                const std::size_t count = std::ranges::size(values);

                if (count == 0 || std::ranges::any_of(values,
                    [](const double value) { return std::isnan(value); }))
                {
                    return not_a_number;
                }

                std::vector<double> values_copy;
                values_copy.reserve(count);
                for (const auto value : values)
                {
                    values_copy.push_back(static_cast<double>(value));
                }

                // Puts the element at `middle` where a full sort would, with nothing larger before it.
                const auto middle = values_copy.begin() + static_cast<std::ptrdiff_t>(values_copy.size() / 2);
                std::ranges::nth_element(values_copy, middle);
                if (values_copy.size() % 2 == 1)
                {
                    return *middle;
                }
                return std::midpoint(*std::max_element(values_copy.begin(), middle), *middle);
            }
        }

        /// NaN for an empty range.
        template <number_range Range>
        [[nodiscard]] double mean(const Range& values)
        {
            if (std::ranges::empty(values))
            {
                return detail::not_a_number;
            }
            return detail::sum_of(values) / static_cast<double>(std::ranges::size(values));
        }

        /// The variance with `ddof` degrees of freedom removed from the count:
        /// 1 for the sample variance, 0 for the population variance. NaN when the
        /// range holds `ddof` values or fewer.
        template <number_range Range>
        [[nodiscard]] double variance(const Range& values, const std::size_t ddof = 1)
        {
            return detail::variance_given_mean(values, mean(values), ddof);
        }

        /// The standard deviation of a variance already in hand: its square root.
        /// NaN stays NaN, and a negative variance, which no data can produce, is NaN.
        [[nodiscard]] inline double standard_deviation(const double variance)
        {
            return std::sqrt(variance);
        }

        /// The square root of variance(), with the same `ddof`.
        template <number_range Range>
        [[nodiscard]] double standard_deviation(const Range& values, const std::size_t ddof = 1)
        {
            return standard_deviation(variance(values, ddof));
        }

        /// The middle value, or the midpoint of the two middle values for an even
        /// count. NaN for an empty range and for one holding a NaN.
        template <number_range Range>
        [[nodiscard]] double median(const Range& values)
        {
            return detail::median_of(values);
        }

        /// Everything a report needs about one series, computed together so the
        /// mean is found once.
        struct Summary
        {
            std::size_t count = 0;
            double mean = detail::not_a_number;
            double std = detail::not_a_number;
            double median = detail::not_a_number;

            /// NaN when any value is NaN, whatever its position.
            double minimum = detail::not_a_number;
            double maximum = detail::not_a_number;
        };

        /// `std` uses `ddof` as in variance().
        template <number_range Range>
        [[nodiscard]] Summary summarize(const Range& values, const std::size_t ddof = 1)
        {
            Summary summary;
            summary.count = std::ranges::size(values);
            if (summary.count == 0)
            {
                return summary;
            }

            summary.mean = mean(values);
            summary.std = standard_deviation(detail::variance_given_mean(values, summary.mean, ddof));
            summary.median = detail::median_of(values);

            auto minimum = static_cast<double>(*std::ranges::begin(values));
            double maximum = minimum;
            for (const auto value : values)
            {
                const auto number = static_cast<double>(value);
                if (std::isnan(number))
                {
                    minimum = number;
                    maximum = number;
                    break;
                }
                minimum = std::min(minimum, number);
                maximum = std::max(maximum, number);
            }
            summary.minimum = minimum;
            summary.maximum = maximum;
            return summary;
        }
    }

    LOSSYLAB_REFLECT(statistics::Summary, count, mean, std, median, minimum, maximum);
}
