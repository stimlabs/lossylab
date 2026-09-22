#include "lossylab/measure/measure.hpp"

#include "lossylab/core/error.hpp"
#include "lossylab/core/json_io.hpp"
#include "lossylab/env/capabilities.hpp"

namespace lossylab
{
    namespace
    {
        /// The libavfilter filter each analyzer is built on. Checking these up
        /// front means a build without one says so by name.
        std::string filter_for(const Analyzer analyzer)
        {
            switch (analyzer)
            {
            case Analyzer::SignalLevels: return "signalstats";
            case Analyzer::Blockiness: return "blockdetect";
            case Analyzer::Blurriness: return "blurdetect";
            case Analyzer::Letterbox: return "cropdetect";
            case Analyzer::Interlacing: return "idet";
            case Analyzer::SpatialTemporalInfo: return "siti";
            case Analyzer::SceneChange: return "scdet";
            case Analyzer::DuplicateFrames: return "mpdecimate";
            }
            return "";
        }

        void pool_into(std::map<std::string, double>& pooled,
                       const std::vector<FrameMeasurement>& frames)
        {
            std::map<std::string, double> totals;
            std::map<std::string, int> counts;

            for (const FrameMeasurement& frame : frames)
            {
                for (const auto& [name, value] : frame.values)
                {
                    totals[name] += value;
                    counts[name] += 1;

                    auto& minimum = pooled[name + "_min"];
                    auto& maximum = pooled[name + "_max"];
                    if (counts[name] == 1)
                    {
                        minimum = value;
                        maximum = value;
                    }
                    else
                    {
                        minimum = std::min(minimum, value);
                        maximum = std::max(maximum, value);
                    }
                }
            }

            for (const auto& [name, total] : totals)
            {
                pooled[name + "_mean"] = total / counts[name];
            }
        }
    }

    std::string to_string(const Analyzer analyzer)
    {
        switch (analyzer)
        {
        case Analyzer::SignalLevels: return "signal_levels";
        case Analyzer::Blockiness: return "blockiness";
        case Analyzer::Blurriness: return "blurriness";
        case Analyzer::Letterbox: return "letterbox";
        case Analyzer::Interlacing: return "interlacing";
        case Analyzer::SpatialTemporalInfo: return "spatial_temporal_info";
        case Analyzer::SceneChange: return "scene_change";
        case Analyzer::DuplicateFrames: return "duplicate_frames";
        }
        return "unknown";
    }

    Analyzer analyzer_from_string(const std::string_view name)
    {
        if (name == "signal_levels") { return Analyzer::SignalLevels; }
        if (name == "blockiness") { return Analyzer::Blockiness; }
        if (name == "blurriness") { return Analyzer::Blurriness; }
        if (name == "letterbox") { return Analyzer::Letterbox; }
        if (name == "interlacing") { return Analyzer::Interlacing; }
        if (name == "spatial_temporal_info") { return Analyzer::SpatialTemporalInfo; }
        if (name == "scene_change") { return Analyzer::SceneChange; }
        if (name == "duplicate_frames") { return Analyzer::DuplicateFrames; }
        throw ConfigError("unknown analyzer '" + std::string(name) + "'");
    }

    std::optional<double> FrameMeasurement::value(const std::string_view name) const
    {
        const auto it = values.find(std::string(name));
        return it == values.end() ? std::nullopt : std::optional<double>(it->second);
    }

    json::Value FrameMeasurement::to_json() const
    {
        json::Value document = json::object({
            {"index", index},
            {"values", json::to_object(values)},
        });

        if (content_rect.has_value())
        {
            document["content_rect"] = content_rect->to_json();
        }
        return document;
    }

    json::Value MeasureResult::to_json() const
    {
        return json::object({
            {"frames", json::to_array(frames)},
            {"pooled", json::to_object(pooled)},
        });
    }

    json::Value CompareResult::to_json() const
    {
        return json::object({
            {"frames", json::to_array(frames, [](const std::map<std::string, double>& metrics) {
                 return json::to_object(metrics);
             })},
            {"pooled", json::to_object(pooled)},
        });
    }

    json::Value RecompressionPoint::to_json() const
    {
        return json::object({
            {"quality_parameter", quality_parameter},
            {"error", error},
            {"bits_per_pixel", bits_per_pixel},
        });
    }

    json::Value RecompressionCurve::to_json() const
    {
        return json::object({
            {"points", json::to_array(points)},
            {"estimated_prior_parameter", json::optional_or_null(estimated_prior_parameter)},
            {"confidence", confidence},
        });
    }

    MeasureResult measure(const std::vector<Frame>& frames,
                          const std::vector<Analyzer>& analyzers)
    {
        if (frames.empty())
        {
            throw ConfigError("measure() received no frames");
        }
        if (analyzers.empty())
        {
            throw ConfigError("measure() received no analyzers");
        }

        // Each analyzer is backed by a libavfilter filter, so a build without
        // one is reported by name before any work starts.
        for (const Analyzer analyzer : analyzers)
        {
            static_cast<void>(capabilities().require_filter(filter_for(analyzer)));
        }

        // Pooling is real and covered by tests; it is the per-frame
        // measurement that still has to be wired to the filters.
        MeasureResult result;
        pool_into(result.pooled, result.frames);

        LL_NOT_IMPLEMENTED();
    }

    MeasureResult measure(const Frame& frame, const std::vector<Analyzer>& analyzers)
    {
        return measure(std::vector<Frame>{frame}, analyzers);
    }

    CompareResult compare(const std::vector<Frame>& reference,
                          const std::vector<Frame>& distorted,
                          const std::vector<Metric>& metrics)
    {
        if (reference.empty() || distorted.empty())
        {
            throw ConfigError("compare() received no frames");
        }
        if (reference.size() != distorted.size())
        {
            throw ConfigError("compare() received " + std::to_string(reference.size()) +
                              " reference frames and " + std::to_string(distorted.size()) +
                              " distorted frames");
        }
        if (metrics.empty())
        {
            throw ConfigError("compare() received no metrics");
        }

        for (std::size_t i = 0; i < reference.size(); ++i)
        {
            if (reference[i].width() != distorted[i].width() ||
                reference[i].height() != distorted[i].height())
            {
                throw ConfigError("compare() frame " + std::to_string(i) +
                                  " differs in size between reference and distorted; "
                                  "resize explicitly first");
            }
        }

        for (const Metric metric : metrics)
        {
            capabilities().require_metric(metric);
        }

        LL_NOT_IMPLEMENTED();
    }

    CompareResult compare(const Frame& reference, const Frame& distorted,
                          const std::vector<Metric>& metrics)
    {
        return compare(std::vector<Frame>{reference}, std::vector<Frame>{distorted}, metrics);
    }

    RecompressionCurve recompression_curve(const Frame& frame,
                                           const RecompressionOptions& options)
    {
        if (frame.empty())
        {
            throw ConfigError("recompression_curve() received an empty frame");
        }
        if (options.parameter_range.size() < 3)
        {
            throw ConfigError("recompression_curve() needs at least three parameters to find "
                              "a minimum");
        }

        static_cast<void>(capabilities().require_encoder(options.codec));
        static_cast<void>(capabilities().require_decoder(options.codec));
        capabilities().require_metric(options.metric);

        LL_NOT_IMPLEMENTED();
    }
}
