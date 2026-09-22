#include "lossylab/core/json_io.hpp"

#include "lossylab/codec/encode.hpp"
#include "lossylab/core/color_spec.hpp"
#include "lossylab/core/geometry.hpp"
#include "lossylab/core/kernel.hpp"
#include "lossylab/core/pipeline_spec.hpp"
#include "lossylab/core/pixel_format.hpp"
#include "lossylab/core/rational.hpp"
#include "lossylab/core/record.hpp"
#include "lossylab/core/strict.hpp"
#include "lossylab/env/build_info.hpp"
#include "lossylab/env/capabilities.hpp"
#include "lossylab/env/log.hpp"
#include "lossylab/io/file_result.hpp"
#include "lossylab/io/probe.hpp"
#include "lossylab/io/read_headers.hpp"
#include "lossylab/measure/measure.hpp"
#include "lossylab/motion/animate_still.hpp"
#include "lossylab/resample/resize.hpp"

namespace lossylab::json
{
    // Every type the library serializes, stated once. A type that grows a
    // to_json without a matching from_json, or loses one of the pair in a
    // refactor, fails here rather than at the call site that needed it.
    static_assert(Serializable<ColorSpec>);
    static_assert(Serializable<Rect>);
    static_assert(Serializable<CoordinateTransform>);
    static_assert(Serializable<BlockGrid>);
    static_assert(Serializable<PixelFormat>);
    static_assert(Serializable<Rational>);
    static_assert(Serializable<KernelParams>);
    static_assert(Serializable<KernelSpec>);
    static_assert(Serializable<ConversionEvent>);
    static_assert(Serializable<FormatDescription>);
    static_assert(Serializable<FrameStats>);
    static_assert(Serializable<StageRecord>);
    static_assert(Serializable<ProcessingRecord>);
    static_assert(Serializable<StageSpec>);
    static_assert(Serializable<PipelineSpec>);
    static_assert(Serializable<TargetSize>);
    static_assert(Serializable<RateControl>);
    static_assert(Serializable<GopStructure>);
    static_assert(Serializable<EncodeTarget>);
    static_assert(Serializable<Trajectory>);
    static_assert(Serializable<TemporalNoise>);
    static_assert(Serializable<FileError>);
    static_assert(Serializable<LogMessage>);

    // Write-only by design: these describe a build, a file or a measurement
    // run, so there is a reader for them but never a parser.
    static_assert(Writable<LibraryVersion>);
    static_assert(Writable<BuildInfo>);
    static_assert(Writable<OptionSchema>);
    static_assert(Writable<CodecInfo>);
    static_assert(Writable<FilterInfo>);
    static_assert(Writable<Capabilities>);
    static_assert(Writable<StreamInfo>);
    static_assert(Writable<ProbeResult>);
    static_assert(Writable<ParameterSet>);
    static_assert(Writable<SliceInfo>);
    static_assert(Writable<HeaderInfo>);
    static_assert(Writable<FrameMeasurement>);
    static_assert(Writable<MeasureResult>);
    static_assert(Writable<CompareResult>);
    static_assert(Writable<RecompressionPoint>);
    static_assert(Writable<RecompressionCurve>);

    std::map<std::string, std::string> to_string_map(const Value& value)
    {
        std::map<std::string, std::string> result;
        for (const auto& [key, member] : value.items())
        {
            result.emplace(key, member.get<std::string>());
        }
        return result;
    }

    std::map<std::string, double> to_double_map(const Value& value)
    {
        std::map<std::string, double> result;
        for (const auto& [key, member] : value.items())
        {
            result.emplace(key, member.get<double>());
        }
        return result;
    }

    std::optional<double> optional_double(const Value& value)
    {
        return value.is_null() ? std::nullopt : std::optional<double>(value.get<double>());
    }

    std::optional<std::int64_t> optional_int(const Value& value)
    {
        return value.is_null() ? std::nullopt
                               : std::optional<std::int64_t>(value.get<std::int64_t>());
    }

    std::optional<int> optional_int32(const Value& value)
    {
        return value.is_null() ? std::nullopt
                               : std::optional<int>(static_cast<int>(value.get<std::int64_t>()));
    }

    std::optional<bool> optional_bool(const Value& value)
    {
        return value.is_null() ? std::nullopt : std::optional<bool>(value.get<bool>());
    }

    std::optional<std::string> optional_string(const Value& value)
    {
        return value.is_null() ? std::nullopt
                               : std::optional<std::string>(value.get<std::string>());
    }
}
