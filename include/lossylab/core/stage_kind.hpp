#pragma once

#include <string>
#include <string_view>

namespace lossylab
{
    /// What kind of operation a stage performed. One member per operation the
    /// design specifies, so a record can be grouped and compared by stage type
    /// across a whole dataset.
    enum class StageKind
    {
        DecodeImage,
        DecodeVideo,
        Convert,
        ChromaRoundtrip,
        Reinterpret,
        Resize,
        Filter,
        EncodeImage,
        EncodeVideo,
        RoundtripImage,
        RoundtripVideo,
        AnimateStill,
        Measure,
        Compare,
        RecompressionCurve,
        CompressionHistory
    };

    std::string to_string(StageKind kind);
    StageKind stage_kind_from_string(std::string_view name);

    inline void from_string(const std::string_view name, StageKind& value)
    {
        value = stage_kind_from_string(name);
    }
}
