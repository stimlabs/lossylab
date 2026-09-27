#include "lossylab/core/pipeline_spec.hpp"

#include "lossylab/core/codec_id.hpp"
#include "lossylab/core/error.hpp"
#include "lossylab/core/json_io.hpp"
#include "lossylab/core/kernel.hpp"
#include "lossylab/core/pixel_format.hpp"
#include "lossylab/env/build_info.hpp"
#include "lossylab/env/capabilities.hpp"

#include <array>
#include <cstdio>
#include <utility>

namespace lossylab
{
    json::Value StageSpec::to_json() const
    {
        json::Value document = json::object({
            {"kind", to_string(kind)},
            {"params", params},
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
        spec.kind = stage_kind_from_string(value.at("kind").get<std::string>());
        spec.params = json::member(value, "params");
        spec.label = json::string_or(value, "label", "");
        return spec;
    }

    PipelineSpec::PipelineSpec(std::vector<StageSpec> stages) : m_stages(std::move(stages)) {}

    PipelineSpec& PipelineSpec::add(StageSpec stage)
    {
        m_stages.push_back(std::move(stage));
        return *this;
    }

    PipelineSpec& PipelineSpec::add(const StageKind kind, json::Value params, std::string label)
    {
        StageSpec stage;
        stage.kind = kind;
        stage.params = std::move(params);
        stage.label = std::move(label);
        return add(std::move(stage));
    }

    namespace
    {
        [[noreturn]] void reject(const std::size_t index, const StageSpec& stage,
                                 const std::string& reason)
        {
            throw ConfigError("pipeline stage " + std::to_string(index) + " (" +
                              to_string(stage.kind) +
                              (stage.label.empty() ? "" : " '" + stage.label + "'") + "): " +
                              reason);
        }

        /// Reads a required string parameter, reporting the stage rather than
        /// just the missing key.
        const std::string& required_string(const std::size_t index, const StageSpec& stage,
                                           const char* key)
        {
            const auto it = stage.params.find(key);
            if (it == stage.params.end() || !it->is_string())
            {
                reject(index, stage, std::string("missing required string parameter '") + key +
                                         "'");
            }
            return it->get_ref<const std::string&>();
        }

        void validate_pixel_format(const std::size_t index, const StageSpec& stage,
                                   const char* key)
        {
            if (!stage.params.contains(key))
            {
                return;
            }
            const std::string& name = required_string(index, stage, key);
            if (!PixelFormat::find(name).has_value())
            {
                reject(index, stage, "unknown pixel format '" + name + "'");
            }
        }

        void validate_stage(const std::size_t index, const StageSpec& stage,
                            const Capabilities& available)
        {
            switch (stage.kind)
            {
            case StageKind::Convert:
            case StageKind::ChromaRoundtrip:
                validate_pixel_format(index, stage, "pix_fmt");
                validate_pixel_format(index, stage, "intermediate_pix_fmt");
                break;

            case StageKind::Resize:
            {
                if (stage.params.contains("kernel"))
                {
                    try
                    {
                        static_cast<void>(
                            kernel_from_string(required_string(index, stage, "kernel")));
                    }
                    catch (const ConfigError& error)
                    {
                        reject(index, stage, error.what());
                    }
                }
                if (stage.params.contains("backend"))
                {
                    const ResizeBackend backend =
                        resize_backend_from_string(required_string(index, stage, "backend"));
                    if (!available.supports(backend))
                    {
                        throw UnsupportedCapability(
                            "resize backend", to_string(backend) + " (pipeline stage " +
                                                  std::to_string(index) + ")",
                            build_info().identity_hash);
                    }
                }
                break;
            }

            case StageKind::Filter:
            {
                // Only the graph's presence is checked here; libavfilter parses
                // the description itself when the graph is compiled.
                static_cast<void>(required_string(index, stage, "graph"));
                break;
            }

            case StageKind::EncodeVideo:
            case StageKind::RoundtripVideo:
            {
                const VideoCodec codec =
                    video_codec_from_string(required_string(index, stage, "codec"));
                EncoderBackend backend = EncoderBackend::Software;
                if (stage.params.contains("backend"))
                {
                    backend =
                        encoder_backend_from_string(required_string(index, stage, "backend"));
                }
                if (!available.supports(codec, backend))
                {
                    throw UnsupportedCapability(
                        "video encoder", to_string(codec) + " (" + to_string(backend) +
                                             ", pipeline stage " + std::to_string(index) + ")",
                        build_info().identity_hash);
                }
                validate_pixel_format(index, stage, "pix_fmt");
                break;
            }

            case StageKind::EncodeImage:
            case StageKind::RoundtripImage:
            {
                const ImageCodec codec =
                    image_codec_from_string(required_string(index, stage, "codec"));
                if (!available.supports(codec))
                {
                    throw UnsupportedCapability(
                        "image encoder",
                        to_string(codec) + " (pipeline stage " + std::to_string(index) + ")",
                        build_info().identity_hash);
                }
                validate_pixel_format(index, stage, "pix_fmt");
                break;
            }

            case StageKind::Compare:
            case StageKind::RecompressionCurve:
            {
                if (stage.params.contains("metric"))
                {
                    const Metric metric =
                        metric_from_string(required_string(index, stage, "metric"));
                    if (!available.supports(metric))
                    {
                        throw UnsupportedCapability(
                            "metric", to_string(metric) + " (pipeline stage " +
                                          std::to_string(index) + ")",
                            build_info().identity_hash);
                    }
                }
                break;
            }

            case StageKind::DecodeImage:
            case StageKind::DecodeVideo:
            case StageKind::Reinterpret:
            case StageKind::AnimateStill:
            case StageKind::Measure:
            case StageKind::CompressionHistory:
                // Nothing build-dependent to check ahead of time.
                break;
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
        // FNV-1a over the serialized form. Object key order is stable by
        // construction, so the same spec always hashes the same.
        const std::string material = to_json().dump();

        std::uint64_t hash = 1469598103934665603ULL;
        for (const char character : material)
        {
            hash ^= static_cast<unsigned char>(character);
            hash *= 1099511628211ULL;
        }

        std::array<char, 32> buffer{};
        std::snprintf(buffer.data(), buffer.size(), "%016llx",
                      static_cast<unsigned long long>(hash));
        return buffer.data();
    }

    json::Value PipelineSpec::to_json() const
    {
        return json::object({{"stages", json::to_array(m_stages)}});
    }

    PipelineSpec PipelineSpec::from_json(const json::Value& value)
    {
        PipelineSpec spec;
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

    bool operator==(const PipelineSpec& left, const PipelineSpec& right)
    {
        return left.to_json() == right.to_json();
    }
}
