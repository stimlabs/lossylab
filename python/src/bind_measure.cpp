#include "bind_reflected.hpp"
#include "bindings.hpp"
#include "json_convert.hpp"

#include "lossylab/measure/compression_history.hpp"
#include "lossylab/measure/measure.hpp"

#include <nanobind/stl/array.h>
#include <nanobind/stl/map.h>
#include <nanobind/stl/optional.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/string_view.h>
#include <nanobind/stl/vector.h>

namespace lossylab::pybind
{
    using namespace nb::literals;

    void bind_measure(nb::module_& m)
    {
        nb::enum_<Analyzer>(m, "Analyzer")
            .value("SignalLevels", Analyzer::SignalLevels)
            .value("Blockiness", Analyzer::Blockiness)
            .value("Blurriness", Analyzer::Blurriness)
            .value("Noise", Analyzer::Noise)
            .value("Letterbox", Analyzer::Letterbox)
            .value("Interlacing", Analyzer::Interlacing)
            .value("SpatialTemporalInfo", Analyzer::SpatialTemporalInfo)
            .value("SceneChange", Analyzer::SceneChange)
            .value("DuplicateFrames", Analyzer::DuplicateFrames);

        bind_reflected_rw<MeasureOptions>(m, "MeasureOptions");

        bind_reflected<statistics::Summary>(m, "Summary");
        bind_reflected<Levels>(m, "Levels");
        bind_reflected<SignalLevels>(m, "SignalLevels");
        bind_reflected<LetterboxBars>(m, "LetterboxBars");
        bind_reflected<Letterbox>(m, "Letterbox");
        bind_reflected<FrameMeasurement>(m, "FrameMeasurement");

        nb::class_<MeasureResult>(m, "MeasureResult")
            .def_ro("frames", &MeasureResult::frames)
            .def_ro("pooled", &MeasureResult::pooled)
            .def_ro("record", &MeasureResult::record)
            .def_prop_ro("configuration", [](const MeasureResult& self) { return to_python(self.configuration); })
            .def("to_dict", [](const MeasureResult& self) { return to_python(self.to_json()); });

        m.def("measure",
              nb::overload_cast<const std::vector<Frame>&, const std::vector<Analyzer>&, const MeasureOptions&>(
                  &measure),
              "frames"_a, "analyzers"_a, "options"_a = MeasureOptions{}, nb::call_guard<nb::gil_scoped_release>());
        m.def("measure",
              nb::overload_cast<const Frame&, const std::vector<Analyzer>&, const MeasureOptions&>(&measure),
              "frame"_a, "analyzers"_a, "options"_a = MeasureOptions{}, nb::call_guard<nb::gil_scoped_release>());

        bind_reflected_rw<CompareOptions>(m, "CompareOptions");

        nb::class_<CompareResult>(m, "CompareResult")
            .def_ro("frames", &CompareResult::frames)
            .def_ro("pooled", &CompareResult::pooled)
            .def_ro("record", &CompareResult::record)
            .def_prop_ro("configuration", [](const CompareResult& self) { return to_python(self.configuration); })
            .def("to_dict", [](const CompareResult& self) { return to_python(self.to_json()); });

        m.def("compare",
              nb::overload_cast<const std::vector<Frame>&, const std::vector<Frame>&, const std::vector<Metric>&,
                                const CompareOptions&>(&compare),
              "reference"_a, "distorted"_a, "metrics"_a, "options"_a = CompareOptions{},
              nb::call_guard<nb::gil_scoped_release>());
        m.def("compare",
              nb::overload_cast<const Frame&, const Frame&, const std::vector<Metric>&, const CompareOptions&>(
                  &compare),
              "reference"_a, "distorted"_a, "metrics"_a, "options"_a = CompareOptions{},
              nb::call_guard<nb::gil_scoped_release>());

        bind_reflected<RecompressionPoint>(m, "RecompressionPoint");
        bind_reflected<RecompressionCurve>(m, "RecompressionCurve");

        nb::enum_<ChromaUpsampling>(m, "ChromaUpsampling")
            .value("Replicate", ChromaUpsampling::Replicate)
            .value("Triangle", ChromaUpsampling::Triangle);

        nb::enum_<TraceEvidence>(m, "TraceEvidence")
            .value("JpegHeader", TraceEvidence::JpegHeader)
            .value("JpegQuantization", TraceEvidence::JpegQuantization)
            .value("ChromaSubsampling", TraceEvidence::ChromaSubsampling)
            .value("Recompression", TraceEvidence::Recompression);

        bind_reflected<QuantizationEstimate>(m, "QuantizationEstimate");
        bind_reflected<JpegQuantizationEvidence>(m, "JpegQuantizationEvidence");
        bind_reflected<ChromaSubsamplingEvidence>(m, "ChromaSubsamplingEvidence");
        bind_reflected<CompressionTrace>(m, "CompressionTrace");
        bind_reflected_rw<CompressionHistoryOptions>(m, "CompressionHistoryOptions");

        bind_reflected<CompressionHistory>(m, "CompressionHistory");

        m.def("compression_history",
              nb::overload_cast<const Frame&, const CompressionHistoryOptions&>(&compression_history), "frame"_a,
              "options"_a = CompressionHistoryOptions{}, nb::call_guard<nb::gil_scoped_release>());
        m.def("compression_history",
              nb::overload_cast<const DecodedImage&, const CompressionHistoryOptions&>(&compression_history),
              "image"_a, "options"_a = CompressionHistoryOptions{}, nb::call_guard<nb::gil_scoped_release>());
    }
}
