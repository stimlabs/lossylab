#include "lossylab/core/rng.hpp"

namespace lossylab
{
    std::uint64_t derive_seed(const std::uint64_t seed,
                              const std::uint64_t stage_index,
                              const std::uint64_t frame_index) noexcept
    {
        // mix() is a bijection with a fixed point at zero, so the offset keeps
        // the common all-zero position (root seed 0, stage 0, frame 0) off it
        // rather than deriving a seed of zero.
        constexpr std::uint64_t domain = 0x243F6A8885A308D3ULL;

        // Each component is mixed separately before being combined, so that
        // neighboring (stage, frame) pairs produce unrelated streams.
        //
        // Integer arithmetic only, and no std::seed_seq: this runs once per
        // stage per frame on every worker, and seed_seq owns a std::vector, so
        // it would allocate on each call.
        std::uint64_t hash = detail::mix(seed ^ domain);
        hash = detail::mix(hash ^ stage_index * detail::golden_gamma);
        hash = detail::mix(hash ^ frame_index * 0xD6E8FEB86659FD93ULL);
        return hash;
    }
}
