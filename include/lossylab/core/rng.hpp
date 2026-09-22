#pragma once

#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>
#include <random>

namespace lossylab
{
    namespace detail
    {
        constexpr std::uint64_t golden_gamma = 0x9E3779B97F4A7C15ULL;

        /// splitmix64's finalizer: a bijection on 64 bits with full avalanche.
        constexpr std::uint64_t mix(std::uint64_t state) noexcept
        {
            state ^= state >> 30;
            state *= 0xBF58476D1CE4E5B9ULL;
            state ^= state >> 27;
            state *= 0x94D049BB133111EBULL;
            state ^= state >> 31;
            return state;
        }

        /// A type usable where the standard engines take a seed sequence.
        template <class Sequence>
        concept seed_sequence =
            requires(Sequence& sequence, std::uint32_t* words) { sequence.generate(words, words); };
    }

    /// splitmix64, as an engine in the standard's sense.
    ///
    /// TODO: replace with `std::philox_engine` once the project moves to
    /// C++26. Philox is counter-based, so it has the same O(1) construction and
    /// no warm-up, and the standard specifies its output values, which removes
    /// the reason this class is hand-written. `next_*` below still has to stay,
    /// since C++26 does not make the distributions portable.
    ///
    /// Models `std::uniform_random_bit_generator`, so it drives `BasicRng`
    /// below and every distribution in `<random>`.
    ///
    /// One word of state, no warm-up, and no allocation, so constructing a
    /// stream costs a single store. Streams here are created per (stage, frame)
    /// on every dataloader worker, which is the case the standard engines are
    /// poorly suited to: seeding `std::mt19937_64` fills 2496 bytes of state
    /// from 624 generated words, so its construction would dominate any stage
    /// that draws only a handful of times. `BulkRng` exists for the stages that
    /// draw enough to amortize that.
    ///
    /// Integer arithmetic only, so a given seed yields the same stream on every
    /// platform. Passes BigCrush; it is the generator the xoshiro family uses
    /// to seed itself.
    class SplitMix64
    {
    public:
        using result_type = std::uint64_t;

        static constexpr result_type default_seed = 0;

        static constexpr result_type(min)() noexcept { return 0; }
        static constexpr result_type(max)() noexcept
        {
            return std::numeric_limits<result_type>::max();
        }

        SplitMix64() noexcept : m_state(default_seed) {}

        explicit SplitMix64(const result_type value) noexcept : m_state(value) {}

        template <detail::seed_sequence SeedSequence>
        explicit SplitMix64(SeedSequence& sequence)
        {
            seed(sequence);
        }

        void seed(const result_type value = default_seed) noexcept { m_state = value; }

        template <detail::seed_sequence SeedSequence>
        void seed(SeedSequence& sequence)
        {
            std::uint32_t words[2] = {};
            sequence.generate(&words[0], &words[2]);
            m_state = static_cast<result_type>(words[1]) << 32 | words[0];
        }

        result_type operator()() noexcept
        {
            m_state += detail::golden_gamma;
            return detail::mix(m_state);
        }

        /// O(1) rather than O(count): the state is a counter.
        void discard(const unsigned long long count) noexcept
        {
            m_state += detail::golden_gamma * static_cast<result_type>(count);
        }

        friend bool operator==(const SplitMix64& left, const SplitMix64& right) noexcept
        {
            return left.m_state == right.m_state;
        }

    private:
        result_type m_state;
    };

    static_assert(std::uniform_random_bit_generator<SplitMix64>);

    /// Seeds any standard engine from a 64-bit value.
    ///
    /// Meets the standard's seed sequence requirements, so it works wherever
    /// `std::seed_seq` does, but holds no storage: `std::seed_seq` keeps a
    /// `std::vector`, so seeding through it allocates on every construction.
    class SeedSource
    {
    public:
        using result_type = std::uint32_t;

        explicit SeedSource(const std::uint64_t seed) noexcept : m_seed(seed) {}

        template <class OutIt>
        void generate(OutIt first, const OutIt last) const
        {
            SplitMix64 stream(detail::mix(m_seed));
            while (first != last)
            {
                const std::uint64_t draw = stream();
                *first++ = static_cast<result_type>(draw);
                if (first == last)
                {
                    break;
                }
                *first++ = static_cast<result_type>(draw >> 32);
            }
        }

    private:
        std::uint64_t m_seed;
    };

    /// A seeded stream over an engine that models
    /// `std::uniform_random_bit_generator`.
    ///
    /// The draws are mapped to values here rather than through
    /// `std::uniform_int_distribution` and friends. The standard pins down the
    /// engines bit for bit but leaves each distribution's algorithm to the
    /// implementation: libstdc++ and libc++ return different values from the
    /// same engine state. A recorded seed has to reproduce a sample on any
    /// machine, so the mapping is spelled out below, using only integer
    /// arithmetic and exactly representable scaling. `next_normal` is the one
    /// exception; see its note.
    ///
    /// Using a `<random>` distribution directly on `engine()` is fine for
    /// anything whose values are not recorded, and gives up that guarantee.
    template <std::uniform_random_bit_generator Engine>
    class BasicRng
    {
    public:
        using engine_type = Engine;

        explicit BasicRng(const std::uint64_t seed) : m_engine(make_engine(seed)) {}

        /// Uniform over the whole 64-bit range.
        std::uint64_t next_u64()
        {
            if constexpr (draw_bits >= 64)
            {
                return static_cast<std::uint64_t>(m_engine() - (Engine::min)());
            }
            else
            {
                // Concatenate whole draws until 64 bits are covered. The last
                // shift may drop bits off the top, which costs nothing here.
                std::uint64_t value = 0;
                for (int filled = 0; filled < 64; filled += draw_bits)
                {
                    value = value << draw_bits
                            | static_cast<std::uint64_t>(m_engine() - (Engine::min)());
                }
                return value;
            }
        }

        /// Uniform in [0, 1).
        double next_double()
        {
            // The top 53 bits are exactly the mantissa width of a double, and
            // 2^-53 is exact, so the scaling introduces no rounding.
            return static_cast<double>(next_u64() >> 11) * (1.0 / 9007199254740992.0);
        }

        /// Uniform in [low, high). Returns `low` for an empty range.
        double next_double(const double low, const double high)
        {
            if (high <= low)
            {
                return low;
            }
            return low + next_double() * (high - low);
        }

        /// Uniform in [low, high], endpoints included. Returns `low` for an
        /// inverted range.
        std::int64_t next_int(const std::int64_t low, const std::int64_t high)
        {
            if (high <= low)
            {
                return low;
            }

            // Unsigned throughout: high - low overflows a signed 64-bit integer
            // once the bounds span more than half the range.
            const auto base = static_cast<std::uint64_t>(low);
            const std::uint64_t span = static_cast<std::uint64_t>(high) - base + 1ULL;
            if (span == 0)
            {
                // The bounds cover all of int64, so every draw is in range.
                return static_cast<std::int64_t>(base + next_u64());
            }

            // Rejection sampling, so the result stays uniform rather than
            // picking up modulo bias at large spans.
            const std::uint64_t limit = ~0ULL - ~0ULL % span;
            std::uint64_t draw = next_u64();
            while (draw >= limit)
            {
                draw = next_u64();
            }
            return static_cast<std::int64_t>(base + draw % span);
        }

        /// Gaussian with the given mean and standard deviation, by Box-Muller.
        ///
        /// Reproducible for a given seed on a given platform, but `std::log`
        /// and `std::cos` are not required to be correctly rounded, so the last
        /// bits can differ between C libraries. This is the only draw here that
        /// is not exact across platforms; do not record one and expect a
        /// bit-identical replay elsewhere.
        double next_normal(const double mean = 0.0, const double stddev = 1.0)
        {
            // 1 - u lands in (0, 1], keeping the logarithm finite. The second
            // Box-Muller variate is discarded rather than cached, so a draw
            // depends only on its position in the stream.
            const double radius = std::sqrt(-2.0 * std::log(1.0 - next_double()));
            const double angle = 2.0 * std::numbers::pi * next_double();
            return mean + stddev * radius * std::cos(angle);
        }

        /// True with the given probability.
        bool next_bool(const double probability = 0.5)
        {
            return next_double() < probability;
        }

        /// The engine itself, for use with any other distribution from
        /// `<random>` (`std::poisson_distribution`, `std::discrete_distribution`,
        /// `std::shuffle`, and so on).
        [[nodiscard]] engine_type& engine() noexcept { return m_engine; }
        [[nodiscard]] const engine_type& engine() const noexcept { return m_engine; }

    private:
        static constexpr auto engine_span =
                static_cast<std::uint64_t>((Engine::max)() - (Engine::min)());

        /// Whole bits available from one draw.
        static constexpr int draw_bits = std::bit_width(engine_span);

        static Engine make_engine(const std::uint64_t seed)
        {
            // Through SeedSource so the whole 64-bit seed reaches the state
            // even when the engine's word size is 32 bits.
            SeedSource source(seed);
            return Engine(source);
        }

        Engine m_engine;
    };

    /// The stream for per-(stage, frame) use: construction is a store.
    using Rng = BasicRng<SplitMix64>;

    /// For a stream that draws enough times to amortize its seeding, such as
    /// one covering a whole plane.
    using BulkRng = BasicRng<std::mt19937_64>;

    /// Derives a stream seed from the root seed and a stage/frame position.
    ///
    /// Stochastic stages draw from `derive_seed(seed, stage_index, frame_index)`
    /// rather than from a shared generator, so a given frame gets the same
    /// numbers whatever the batch size, thread count or execution order. That
    /// is what makes a recorded seed enough to reproduce a sample exactly.
    ///
    /// `frame_index` has to identify the sample itself, not a per-worker
    /// counter: a counter makes the draws depend on how work was distributed,
    /// which is the collision this indirection exists to avoid.
    [[nodiscard]] std::uint64_t derive_seed(std::uint64_t seed,
                                            std::uint64_t stage_index,
                                            std::uint64_t frame_index) noexcept;
}
