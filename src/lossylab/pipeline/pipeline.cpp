#include "lossylab/pipeline/pipeline.hpp"

#include "lossylab/codec/encode.hpp"
#include "lossylab/convert/convert.hpp"
#include "lossylab/core/error.hpp"
#include "lossylab/env/capabilities.hpp"
#include "lossylab/io/decode_image.hpp"
#include "lossylab/transform/transform.hpp"

#include <type_traits>
#include <utility>

namespace lossylab
{
    struct Pipeline::Impl
    {
        PipelineSpec spec;
    };

    namespace
    {
        /// Runs one stage on `frame`.
        FrameResult run_stage(const Frame& frame, const StageConfiguration& configuration,
                              const ProcessingRecord& record)
        {
            return std::visit(
                [&](const auto& options) -> FrameResult
                {
                    using Options = std::decay_t<decltype(options)>;
                    if constexpr (std::is_same_v<Options, ConvertOptions>)
                    {
                        return convert(frame, options);
                    }
                    else if constexpr (std::is_same_v<Options, ChromaRoundtripOptions>)
                    {
                        return chroma_roundtrip(frame, options);
                    }
                    else if constexpr (std::is_same_v<Options, ReinterpretOptions>)
                    {
                        return reinterpret(frame, options);
                    }
                    else if constexpr (std::is_same_v<Options, CropOptions>)
                    {
                        return crop(frame, options, record.effective_block_grid());
                    }
                    else if constexpr (std::is_same_v<Options, OrientOptions>)
                    {
                        return orient(frame, options);
                    }
                    else if constexpr (std::is_same_v<Options, AchromaticOptions>)
                    {
                        return achromatic(frame, options);
                    }
                    else if constexpr (std::is_same_v<Options, RoundtripImageConfiguration>)
                    {
                        DecodeSpec decode_spec;
                        decode_spec.conversion = options.decode.conversion;
                        decode_spec.strict = options.decode.strict;
                        return roundtrip(frame, options.encode, decode_spec);
                    }
                    else
                    {
                        throw ConfigError("a " + to_string(StageKindOf<Options>::value) +
                                          " stage does not run in a pipeline");
                    }
                },
                configuration);
        }

        /// The record a run starts from: with the build and machine for a
        /// full record, empty for a lean one.
        ProcessingRecord initial_record(const RecordDetail detail)
        {
            return detail == RecordDetail::Full ? ProcessingRecord::for_this_build() : ProcessingRecord{};
        }

        /// Runs the stages from `first` on, then states the seed and, for a
        /// full record, the output's hash.
        PipelineResult run_stages(const PipelineSpec& spec, const std::size_t first, Frame frame,
                                  ProcessingRecord record, const std::uint64_t seed, const RecordDetail detail)
        {
            for (std::size_t index = first; index < spec.size(); ++index)
            {
                FrameResult result = run_stage(frame, spec.stages()[index].configuration, record);
                record.append(std::move(result.record), std::move(result.configuration));
                frame = std::move(result.frame);
            }
            record.validate_continuity();
            record.set_seed(seed);
            if (detail == RecordDetail::Full)
            {
                record.set_output_sha256(frame.samples_sha256());
            }
            return PipelineResult{std::move(frame), std::move(record)};
        }
    }

    Pipeline::Pipeline(PipelineSpec spec) : m_impl(std::make_unique<Impl>())
    {
        // Validated at construction rather than at run time: a spec that this
        // build cannot execute should fail before a dataset starts, not on
        // sample forty thousand with half the data already written.
        spec.validate(capabilities());
        m_impl->spec = std::move(spec);
    }

    Pipeline::~Pipeline() = default;
    Pipeline::Pipeline(Pipeline&&) noexcept = default;
    Pipeline& Pipeline::operator=(Pipeline&&) noexcept = default;

    const PipelineSpec& Pipeline::spec() const noexcept
    {
        return m_impl->spec;
    }

    PipelineResult Pipeline::run(const Source& source, const std::uint64_t seed, const RecordDetail detail) const
    {
        const PipelineSpec& spec = m_impl->spec;
        const auto* decode_options = std::get_if<DecodeImageOptions>(&spec.stages().front().configuration);
        if (decode_options == nullptr)
        {
            throw ConfigError("Pipeline::run() on a source needs a decode as the first stage");
        }
        if (spec.source_sha256().has_value() && *spec.source_sha256() != source.sha256())
        {
            throw ConfigError("the pipeline spec was written for " + *spec.source_sha256() + ", not for " +
                              source.sha256());
        }
        DecodedImage decoded = decode_image(source, *decode_options);
        ProcessingRecord record = initial_record(detail);
        record.set_origin(std::move(decoded.probe));
        record.append(std::move(decoded.record), std::move(decoded.configuration));
        return run_stages(spec, 1, std::move(decoded.frame), std::move(record), seed, detail);
    }

    PipelineResult Pipeline::run(const Frame& frame, const std::uint64_t seed, const RecordDetail detail) const
    {
        const PipelineSpec& spec = m_impl->spec;
        if (frame.empty())
        {
            throw ConfigError("Pipeline::run() received an empty frame");
        }
        if (spec.stages().front().kind() == StageKind::DecodeImage)
        {
            throw ConfigError("Pipeline::run() on a frame cannot start with a decode");
        }
        return run_stages(spec, 0, frame, initial_record(detail), seed, detail);
    }
}
