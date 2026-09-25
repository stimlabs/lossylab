#include "lossylab/core/statistics.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <limits>
#include <numeric>
#include <random>
#include <ranges>
#include <vector>

using namespace lossylab;

namespace
{
    constexpr double not_a_number = std::numeric_limits<double>::quiet_NaN();
    constexpr double infinity = std::numeric_limits<double>::infinity();

    void test_mean_of_doubles_and_integers()
    {
        assert(statistics::mean(std::vector<double>{60.0, 100.0, 140.0}) == 100.0);
        assert(statistics::mean(std::vector<int>{1, 2, 4}) == 7.0 / 3.0);
    }

    void test_mean_of_a_subrange()
    {
        const std::vector<double> values{1.0, 3.0, 100.0};
        assert(statistics::mean(std::ranges::subrange(values.begin(), values.begin() + 2)) == 2.0);
    }

    void test_the_sum_is_pairwise_so_a_long_series_keeps_its_precision()
    {
        const std::vector<double> values(1'000'000, 0.1);
        const double naive_mean =
            std::accumulate(values.begin(), values.end(), 0.0) / static_cast<double>(values.size());
        assert(std::abs(naive_mean - 0.1) > 1e-13);
        assert(std::abs(statistics::mean(values) - 0.1) < 1e-13);
        assert(statistics::variance(values) < 1e-25);
    }

    void test_variance_takes_its_degrees_of_freedom_from_ddof()
    {
        const std::vector<double> values{2, 4, 4, 4, 5, 5, 7, 9};
        assert(std::abs(statistics::variance(values, 0) - 4.0) < 1e-12);
        assert(std::abs(statistics::variance(values) - 32.0 / 7.0) < 1e-12);
        assert(std::abs(statistics::standard_deviation(values, 0) - 2.0) < 1e-12);
        assert(std::abs(statistics::standard_deviation(values) - std::sqrt(32.0 / 7.0)) < 1e-12);
        assert(statistics::standard_deviation(std::vector<double>{3.0, 3.0}) == 0.0);
    }

    void test_standard_deviation_of_a_given_variance_is_its_square_root()
    {
        assert(statistics::standard_deviation(0.0) == 0.0);
        assert(statistics::standard_deviation(9.0) == 3.0);
        assert(statistics::standard_deviation(2.25) == 1.5);
        assert(statistics::standard_deviation(infinity) == infinity);
        assert(std::isnan(statistics::standard_deviation(not_a_number)));
        assert(std::isnan(statistics::standard_deviation(-1.0)));
    }

    void test_standard_deviation_of_values_goes_through_their_variance()
    {
        const std::vector<double> values{2, 4, 4, 4, 5, 5, 7, 9};
        assert(statistics::standard_deviation(values) == statistics::standard_deviation(statistics::variance(values)));
        assert(statistics::standard_deviation(values, 0) ==
               statistics::standard_deviation(statistics::variance(values, 0)));
    }

    void test_median_of_odd_and_even_counts()
    {
        assert(statistics::median(std::vector<double>{9.0, 1.0, 5.0}) == 5.0);
        assert(statistics::median(std::vector<double>{4.0, 1.0, 3.0, 2.0}) == 2.5);
        assert(statistics::median(std::vector<int>{7}) == 7.0);
    }

    void test_the_median_of_a_shuffled_series_needs_no_full_sort()
    {
        std::vector<int> odd(1001);
        std::iota(odd.begin(), odd.end(), 1);
        std::vector<int> even(1000);
        std::iota(even.begin(), even.end(), 1);

        std::mt19937 generator(5);
        std::ranges::shuffle(odd, generator);
        std::ranges::shuffle(even, generator);
        assert(statistics::median(odd) == 501.0);
        assert(statistics::median(even) == 500.5);
    }

    void test_the_median_midpoint_does_not_overflow()
    {
        const double largest = std::numeric_limits<double>::max();
        assert(statistics::median(std::vector<double>{largest, largest}) == largest);
    }

    void test_median_leaves_its_input_unsorted()
    {
        const std::vector<double> values{3.0, 1.0, 2.0};
        assert(statistics::median(values) == 2.0);
        assert(values == (std::vector<double>{3.0, 1.0, 2.0}));
    }

    void test_undefined_results_are_nan()
    {
        const std::vector<double> empty;
        assert(std::isnan(statistics::mean(empty)));
        assert(std::isnan(statistics::median(empty)));
        assert(std::isnan(statistics::variance(empty)));

        const std::vector<double> single{1.0};
        assert(std::isnan(statistics::standard_deviation(single)));
        assert(statistics::standard_deviation(single, 0) == 0.0);
        assert(std::isnan(statistics::standard_deviation(single, 1)));
        assert(std::isnan(statistics::standard_deviation(single, 2)));
    }

    void test_nan_and_infinity_propagate()
    {
        const std::vector<double> with_nan{1.0, not_a_number, 3.0};
        assert(std::isnan(statistics::mean(with_nan)));
        assert(std::isnan(statistics::median(with_nan)));
        assert(std::isnan(statistics::standard_deviation(with_nan)));

        const std::vector<double> with_infinity{1.0, infinity};
        assert(statistics::mean(with_infinity) == infinity);
        assert(std::isnan(statistics::standard_deviation(with_infinity)));
    }

    void test_summarize_agrees_with_the_single_functions()
    {
        const std::vector<double> values{5.0, 1.0, 4.0, 2.0, 8.0, 3.0};
        const statistics::Summary summary = statistics::summarize(values);
        assert(summary.count == 6);
        assert(summary.mean == statistics::mean(values));
        assert(summary.std == statistics::standard_deviation(values));
        assert(summary.median == statistics::median(values));
        assert(summary.minimum == 1.0);
        assert(summary.maximum == 8.0);
        assert(statistics::summarize(values, 0).std == statistics::standard_deviation(values, 0));
    }

    void test_summarize_extremes_propagate_nan_from_any_position()
    {
        for (const std::vector<double>& values : {std::vector<double>{not_a_number, 1.0, 2.0},
                                                  std::vector<double>{1.0, not_a_number, 2.0},
                                                  std::vector<double>{1.0, 2.0, not_a_number}})
        {
            const statistics::Summary summary = statistics::summarize(values);
            assert(std::isnan(summary.minimum));
            assert(std::isnan(summary.maximum));
        }
    }

    void test_summarize_of_nothing_is_all_nan()
    {
        const statistics::Summary summary = statistics::summarize(std::vector<double>{});
        assert(summary.count == 0);
        assert(std::isnan(summary.mean));
        assert(std::isnan(summary.std));
        assert(std::isnan(summary.median));
        assert(std::isnan(summary.minimum));
        assert(std::isnan(summary.maximum));
    }
}

int main()
{
    test_mean_of_doubles_and_integers();
    test_mean_of_a_subrange();
    test_the_sum_is_pairwise_so_a_long_series_keeps_its_precision();
    test_variance_takes_its_degrees_of_freedom_from_ddof();
    test_standard_deviation_of_a_given_variance_is_its_square_root();
    test_standard_deviation_of_values_goes_through_their_variance();
    test_median_of_odd_and_even_counts();
    test_the_median_of_a_shuffled_series_needs_no_full_sort();
    test_the_median_midpoint_does_not_overflow();
    test_median_leaves_its_input_unsorted();
    test_undefined_results_are_nan();
    test_nan_and_infinity_propagate();
    test_summarize_agrees_with_the_single_functions();
    test_summarize_extremes_propagate_nan_from_any_position();
    test_summarize_of_nothing_is_all_nan();
}
