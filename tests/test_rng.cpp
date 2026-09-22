#include "lossylab/core/rng.hpp"

#include <array>
#include <cassert>
#include <random>
#include <set>

using namespace lossylab;

// ---------------------------------------------------------------------------
// Cross-platform reproducibility
//
// The values below are not "whatever the implementation happened to produce".
// SplitMix64, SeedSource, derive_seed and the next_* mapping are integer
// arithmetic and exactly representable scaling throughout, and std::mt19937_64
// is specified bit for bit by the standard. So every platform owes these exact
// numbers, and a recorded seed replays a sample anywhere. A failure here means
// a replay would silently differ.
//
// Deliberately absent: next_normal. It goes through std::log and std::cos,
// which are not required to be correctly rounded, so it is pinned by
// distribution rather than by value.
// ---------------------------------------------------------------------------

namespace
{
    void test_u64_draws_match_recorded_values()
    {
        Rng rng(0x0123456789ABCDEFULL);
        const std::array<std::uint64_t, 4> expected{
            2243615424583747242ULL,
            2443229808746324235ULL,
            9568558537911005301ULL,
            664456375205090465ULL,
        };
        for (const std::uint64_t want : expected)
        {
            assert(rng.next_u64() == want);
        }
    }

    void test_double_draws_match_recorded_values()
    {
        // Exact comparison, not an epsilon check: 17 significant digits determine
        // a double uniquely, and the mapping from draw to value is exact.
        Rng rng(0x0123456789ABCDEFULL);
        const std::array<double, 4> expected{
            0.12162663587778422,
            0.13244775332620540,
            0.51871259771789169,
            0.036020252275960063,
        };
        for (const double want : expected)
        {
            assert(rng.next_double() == want);
        }
    }

    void test_int_draws_match_recorded_values()
    {
        Rng rng(7);
        const std::array<std::int64_t, 4> expected{-623, 750, 106, 105};
        for (const std::int64_t want : expected)
        {
            assert(rng.next_int(-1000, 1000) == want);
        }
    }

    void test_bool_draws_match_recorded_values()
    {
        Rng rng(42);
        const std::array<bool, 8> expected{true, false, false, false, false, true, false, true};
        for (const bool want : expected)
        {
            assert(rng.next_bool(0.5) == want);
        }
    }

    void test_bulk_draws_match_recorded_values()
    {
        // BulkRng goes through std::mt19937_64 seeded from SeedSource, so it is
        // pinned too: a stage may record a seed and replay it through either alias.
        BulkRng rng(0x0123456789ABCDEFULL);
        assert(rng.next_u64() == 3532592993411665024ULL);
        assert(rng.next_u64() == 8113767177791018731ULL);
    }

    void test_derived_seeds_match_recorded_values()
    {
        assert(derive_seed(0xABCDEF, 2, 7) == 15447120142062211791ULL);
        assert(derive_seed(0, 0, 0) == 4200910347100223505ULL);
        assert(derive_seed(1, 0, 0) == 4444877964660223817ULL);
    }

    void test_the_all_zero_position_does_not_derive_a_zero_seed()
    {
        // mix() is a bijection with a fixed point at zero, so without the domain
        // offset in derive_seed the most ordinary position of all -- an unset root
        // seed at stage 0, frame 0 -- would collapse to a seed of zero.
        assert(derive_seed(0, 0, 0) != std::uint64_t{0});
    }

    void test_same_seed_gives_the_same_stream()
    {
        Rng a(12345);
        Rng b(12345);
        for (int i = 0; i < 100; ++i)
        {
            assert(a.next_u64() == b.next_u64());
        }
    }

    void test_different_seeds_diverge()
    {
        Rng a(1);
        Rng b(2);
        assert(a.next_u64() != b.next_u64());
    }

    void test_doubles_stay_in_range()
    {
        Rng rng(99);
        for (int i = 0; i < 10000; ++i)
        {
            const double value = rng.next_double();
            assert(value >= 0.0 && value < 1.0);
        }
        for (int i = 0; i < 1000; ++i)
        {
            const double value = rng.next_double(-3.0, 5.0);
            assert(value >= -3.0 && value < 5.0);
        }
    }

    void test_ints_stay_in_range_and_cover_it()
    {
        Rng rng(7);
        std::set<std::int64_t> seen;
        for (int i = 0; i < 2000; ++i)
        {
            const std::int64_t value = rng.next_int(10, 15);
            assert(value >= 10 && value <= 15);
            seen.insert(value);
        }
        // All six values should appear; a broken range would miss an endpoint.
        assert(seen.size() == std::size_t{6});
    }

    void test_degenerate_int_range_returns_the_bound()
    {
        Rng rng(1);
        assert(rng.next_int(5, 5) == 5);
        assert(rng.next_int(5, 1) == 5);
    }

    void test_normal_draws_center_on_the_mean()
    {
        Rng rng(2024);
        double sum = 0.0;
        constexpr int n = 20000;
        for (int i = 0; i < n; ++i)
        {
            sum += rng.next_normal(4.0, 2.0);
        }
        // Loose enough not to be flaky, tight enough to catch a swapped or ignored
        // mean; the standard error here is 2.0 / sqrt(20000), about 0.014.
        const double mean = sum / n;
        assert(mean > 3.8 && mean < 4.2);
    }

    void test_bernoulli_respects_its_probability()
    {
        Rng rng(31337);
        int hits = 0;
        constexpr int n = 20000;
        for (int i = 0; i < n; ++i)
        {
            hits += rng.next_bool(0.25) ? 1 : 0;
        }
        assert(hits > n / 5 && hits < n / 3);

        // The degenerate probabilities must not draw at random at all.
        for (int i = 0; i < 100; ++i)
        {
            assert(rng.next_bool(1.0));
            assert(!rng.next_bool(0.0));
        }
    }

    void test_splitmix64_behaves_like_a_standard_engine()
    {
        // Modeling the engine requirements is what lets SplitMix64 drive both
        // BasicRng and any <random> distribution; the concept itself is checked by
        // a static_assert in the header.
        SplitMix64 a(9);
        SplitMix64 b(9);
        assert(a == b);

        (void)a();
        assert(a != b);

        b.seed(9);
        assert(a != b);
        a.seed(9);
        assert(a == b);

        assert((SplitMix64::min)() == std::uint64_t{0});
        assert((SplitMix64::max)() == ~std::uint64_t{0});
    }

    void test_discarding_matches_drawing_and_throwing_away()
    {
        // discard is O(1) here because the state is a counter, so it is worth
        // checking it lands exactly where the draws would have.
        SplitMix64 skipped(1234);
        SplitMix64 drawn(1234);

        skipped.discard(1000);
        for (int i = 0; i < 1000; ++i)
        {
            (void)drawn();
        }
        assert(skipped == drawn);
        assert(skipped() == drawn());
    }

    void test_seed_source_fills_any_length()
    {
        // Two 32-bit words come out of each 64-bit draw, so an odd length exercises
        // the half-used final draw.
        const SeedSource source(0xFEEDFACEULL);

        std::array<std::uint32_t, 5> a{};
        std::array<std::uint32_t, 5> b{};
        source.generate(a.begin(), a.end());
        source.generate(b.begin(), b.end());
        assert(a == b);

        std::set<std::uint32_t> distinct(a.begin(), a.end());
        assert(distinct.size() == std::size_t{5});
    }

    void test_a_narrower_engine_still_fills_64_bits()
    {
        // std::mt19937 yields 32 bits per draw, so next_u64 has to concatenate
        // two of them. A single draw would leave the top half always zero.
        BasicRng<std::mt19937> a(4242);
        BasicRng<std::mt19937> b(4242);

        std::uint64_t high_bits_seen = 0;
        for (int i = 0; i < 64; ++i)
        {
            const std::uint64_t value = a.next_u64();
            assert(value == b.next_u64());
            high_bits_seen |= value >> 32;
        }
        assert(high_bits_seen != std::uint64_t{0});
    }

    void test_the_engine_is_usable_with_other_distributions()
    {
        // The point of exposing engine(): a stage can reach for any distribution in
        // <random> without a new wrapper method.
        Rng rng(5);
        std::poisson_distribution<int> poisson(3.0);
        for (int i = 0; i < 1000; ++i)
        {
            assert(poisson(rng.engine()) >= 0);
        }
    }

    void test_derived_seeds_are_position_dependent_not_order_dependent()
    {
        // This is what makes a recorded seed enough to reproduce one sample: a
        // given (stage, frame) draws the same stream whatever the batch size,
        // thread count or order of execution.
        constexpr std::uint64_t root = 0xABCDEF;

        assert(derive_seed(root, 2, 7) == derive_seed(root, 2, 7));
        assert(derive_seed(root, 2, 7) != derive_seed(root, 7, 2));
        assert(derive_seed(root, 2, 7) != derive_seed(root, 2, 8));
        assert(derive_seed(root, 2, 7) != derive_seed(root + 1, 2, 7));
    }

    void test_neighboring_positions_produce_unrelated_seeds()
    {
        // Adjacent stage and frame indices are the common case, so they must not
        // produce correlated streams.
        std::set<std::uint64_t> seeds;
        for (std::uint64_t stage = 0; stage < 16; ++stage)
        {
            for (std::uint64_t frame = 0; frame < 16; ++frame)
            {
                seeds.insert(derive_seed(0, stage, frame));
            }
        }
        assert(seeds.size() == std::size_t{256});
    }

    void test_zero_seed_is_not_degenerate()
    {
        // A seed of zero still produces a live stream; a generator that got stuck
        // at zero would silently disable every stochastic stage.
        Rng rng(0);
        assert(rng.next_u64() != std::uint64_t{0});
        assert(rng.next_u64() != rng.next_u64());
    }
}

int main()
{
    test_u64_draws_match_recorded_values();
    test_double_draws_match_recorded_values();
    test_int_draws_match_recorded_values();
    test_bool_draws_match_recorded_values();
    test_bulk_draws_match_recorded_values();
    test_derived_seeds_match_recorded_values();
    test_the_all_zero_position_does_not_derive_a_zero_seed();
    test_same_seed_gives_the_same_stream();
    test_different_seeds_diverge();
    test_doubles_stay_in_range();
    test_ints_stay_in_range_and_cover_it();
    test_degenerate_int_range_returns_the_bound();
    test_normal_draws_center_on_the_mean();
    test_bernoulli_respects_its_probability();
    test_splitmix64_behaves_like_a_standard_engine();
    test_discarding_matches_drawing_and_throwing_away();
    test_seed_source_fills_any_length();
    test_a_narrower_engine_still_fills_64_bits();
    test_the_engine_is_usable_with_other_distributions();
    test_derived_seeds_are_position_dependent_not_order_dependent();
    test_neighboring_positions_produce_unrelated_seeds();
    test_zero_seed_is_not_degenerate();
}
