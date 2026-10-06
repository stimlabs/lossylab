#include "lossylab/core/pipeline_spec.hpp"

#include "lossylab/core/error.hpp"
#include "lossylab/core/json_io.hpp"
#include "lossylab/detail/ff_ptr.hpp"
#include "lossylab/env/build_info.hpp"
#include "lossylab/env/capabilities.hpp"

#include <cstdint>
#include <span>
#include <utility>

namespace lossylab
{
    json::Value StageSpec::to_json() const
    {
        json::Value document = json::object({
            {"kind", to_string(kind())},
            {"configuration", configuration_to_json(configuration)},
        });
        if (!label.empty())
        {
            document["label"] = label;
        }
        return document;
    }

    StageSpec StageSpec::from_json(const json::Value& value)
    {
        StageSpec spec;
        spec.configuration = configuration_from_json(stage_kind_from_string(value.at("kind").get<std::string>()),
                                                     value.at("configuration"));
        spec.label = json::string_or(value, "label", "");
        return spec;
    }

    PipelineSpec::PipelineSpec(std::vector<StageSpec> stages) : m_stages(std::move(stages)) {}

    PipelineSpec& PipelineSpec::add(StageSpec stage)
    {
        m_stages.push_back(std::move(stage));
        return *this;
    }

    PipelineSpec& PipelineSpec::add(StageConfiguration configuration, std::string label)
    {
        return add(StageSpec{std::move(configuration), std::move(label)});
    }

    namespace
    {
        [[noreturn]] void reject(const std::size_t index, const StageSpec& stage, const std::string& reason)
        {
            throw ConfigError("pipeline stage " + std::to_string(index) + " (" + to_string(stage.kind()) +
                              (stage.label.empty() ? "" : " '" + stage.label + "'") + "): " + reason);
        }

        void require_backend(const std::size_t index, const ResizeBackend backend, const Capabilities& available)
        {
            if (!available.supports(backend))
            {
                throw UnsupportedCapability("resize backend",
                                            to_string(backend) + " (pipeline stage " + std::to_string(index) + ")",
                                            build_info().identity_hash);
            }
        }

        void validate_stage(const std::size_t index, const StageSpec& stage, const Capabilities& available)
        {
            switch (stage.kind())
            {
            case StageKind::DecodeImage:
                if (index != 0)
                {
                    reject(index, stage, "a decode can only be the first stage");
                }
                if (const auto& conversion = std::get<DecodeImageOptions>(stage.configuration).conversion)
                {
                    require_backend(index, conversion->backend, available);
                }
                break;

            case StageKind::Convert:
                require_backend(index, std::get<ConvertOptions>(stage.configuration).backend, available);
                break;

            case StageKind::ChromaRoundtrip:
                require_backend(index, std::get<ChromaRoundtripOptions>(stage.configuration).backend, available);
                break;

            case StageKind::RoundtripImage:
            {
                const ImageCodec codec = std::get<RoundtripImageConfiguration>(stage.configuration).encode.codec;
                if (!available.supports(codec))
                {
                    throw UnsupportedCapability("image encoder",
                                                to_string(codec) + " (pipeline stage " + std::to_string(index) + ")",
                                                build_info().identity_hash);
                }
                break;
            }

            case StageKind::Reinterpret:
            case StageKind::Crop:
            case StageKind::Orient:
            case StageKind::Achromatic:
                break;

            default:
                reject(index, stage, "this kind of stage does not run in a pipeline");
            }
        }
    }

    void PipelineSpec::validate(const Capabilities& capabilities) const
    {
        if (m_stages.empty())
        {
            throw ConfigError("pipeline spec has no stages");
        }
        for (std::size_t i = 0; i < m_stages.size(); ++i)
        {
            validate_stage(i, m_stages[i], capabilities);
        }
    }

    void PipelineSpec::validate() const
    {
        validate(lossylab::capabilities());
    }

    std::string PipelineSpec::spec_id() const
    {
        const std::string material = to_json().dump();
        return detail::sha256_hex(
            std::span(reinterpret_cast<const std::uint8_t*>(material.data()), material.size()));
    }

    json::Value PipelineSpec::to_json() const
    {
        return json::object({
            {"source_sha256", m_source_sha256.has_value() ? json::Value(*m_source_sha256) : json::Value()},
            {"stages", json::to_array(m_stages)},
        });
    }

    PipelineSpec PipelineSpec::from_json(const json::Value& value)
    {
        PipelineSpec spec;
        if (const json::Value& source = json::member(value, "source_sha256"); !source.is_null())
        {
            spec.m_source_sha256 = source.get<std::string>();
        }
        for (StageSpec& stage : json::from_array<StageSpec>(value.at("stages")))
        {
            spec.add(std::move(stage));
        }
        return spec;
    }

    PipelineSpec PipelineSpec::parse(const std::string_view text)
    {
        return from_json(json::parse(text));
    }

    PipelineSpec PipelineSpec::from_record(const ProcessingRecord& record)
    {
        PipelineSpec spec;
        for (const StageConfiguration& configuration : record.configurations())
        {
            spec.add(configuration);
        }
        if (!record.empty())
        {
            if (const auto* decode = std::get_if<DecodeImageEvidence>(&record.stages().front().evidence))
            {
                spec.m_source_sha256 = decode->source_sha256;
            }
        }
        return spec;
    }

    bool operator==(const PipelineSpec& left, const PipelineSpec& right)
    {
        return left.to_json() == right.to_json();
    }
}
