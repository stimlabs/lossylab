#include "lossylab/core/result.hpp"

namespace lossylab
{
    double EncodedResult::bits_per_pixel() const noexcept
    {
        const std::int64_t pixels = static_cast<std::int64_t>(record.output.width) *
                                    static_cast<std::int64_t>(record.output.height) *
                                    static_cast<std::int64_t>(std::max<std::size_t>(
                                        record.frames.size(), 1));
        if (pixels <= 0)
        {
            return 0.0;
        }
        return static_cast<double>(bytes.size()) * 8.0 / static_cast<double>(pixels);
    }
}
