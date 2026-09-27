#include "bind_reflected.hpp"
#include "bindings.hpp"
#include "json_convert.hpp"

#include "lossylab/measure/compression_history.hpp"
#include "lossylab/measure/measure.hpp"

#include <nanobind/stl/array.h>
#include <nanobind/stl/string_view.h>

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
        bind_reflected<MeasureEvidence>(m, "MeasureEvidence");

        nb::class_<MeasureResult>(m, "MeasureResult")
            .def_ro("record", &MeasureResult::record)
            .def_ro("configuration", &MeasureResult::configuration)
            .def_prop_ro("evidence", &MeasureResult::evidence, nb::rv_policy::reference_internal,
                         "The measurements: per frame, pooled, and the format each analyzer measured in.")
            .def("to_dict", [](const MeasureResult& self) { return to_python(self.to_json()); });

        m.def("measure", nb::overload_cast<const std::vector<Frame>&, const MeasureOptions&>(&measure), "frames"_a,
              "options"_a, nb::call_guard<nb::gil_scoped_release>());
        m.def("measure", nb::overload_cast<const Frame&, const MeasureOptions&>(&measure), "frame"_a, "options"_a,
              nb::call_guard<nb::gil_scoped_release>());
        m.def("measure", nb::overload_cast<const std::vector<Frame>&, const std::vector<Analyzer>&>(&measure),
              "frames"_a, "analyzers"_a, nb::call_guard<nb::gil_scoped_release>());
        m.def("measure", nb::overload_cast<const Frame&, const std::vector<Analyzer>&>(&measure), "frame"_a,
              "analyzers"_a, nb::call_guard<nb::gil_scoped_release>());

        bind_reflected_rw<CompareOptions>(m, "CompareOptions");
        bind_reflected<CompareEvidence>(m, "CompareEvidence");

        nb::class_<CompareResult>(m, "CompareResult")
            .def_ro("record", &CompareResult::record)
            .def_ro("configuration", &CompareResult::configuration)
            .def_prop_ro("evidence", &CompareResult::evidence, nb::rv_policy::reference_internal,
                         "The metric values: per frame, pooled, and the format each metric measured in.")
            .def("to_dict", [](const CompareResult& self) { return to_python(self.to_json()); });

        m.def("compare",
              nb::overload_cast<const std::vector<Frame>&, const std::vector<Frame>&, const CompareOptions&>(
                  &compare),
              "reference"_a, "distorted"_a, "options"_a, nb::call_guard<nb::gil_scoped_release>());
        m.def("compare", nb::overload_cast<const Frame&, const Frame&, const CompareOptions&>(&compare),
              "reference"_a, "distorted"_a, "options"_a, nb::call_guard<nb::gil_scoped_release>());
        m.def("compare",
              nb::overload_cast<const std::vector<Frame>&, const std::vector<Frame>&, const std::vector<Metric>&>(
                  &compare),
              "reference"_a, "distorted"_a, "metrics"_a, nb::call_guard<nb::gil_scoped_release>());
        m.def("compare", nb::overload_cast<const Frame&, const Frame&, const std::vector<Metric>&>(&compare),
              "reference"_a, "distorted"_a, "metrics"_a, nb::call_guard<nb::gil_scoped_release>());

        bind_reflected<RecompressionPoint>(m, "RecompressionPoint");
        bind_reflected<RecompressionCurveEvidence>(m, "RecompressionCurveEvidence");

        nb::class_<RecompressionCurve>(m, "RecompressionCurve")
            .def_ro("record", &RecompressionCurve::record)
            .def_ro("configuration", &RecompressionCurve::configuration)
            .def_prop_ro("evidence", &RecompressionCurve::evidence, nb::rv_policy::reference_internal,
                         "The curve: its points, its notch and how confident it is.")
            .def("to_dict", [](const RecompressionCurve& self) { return to_python(self.to_json()); });

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
        bind_reflected<RecompressionSweep>(m, "RecompressionSweep");
        bind_reflected<JpegTableAgreement>(m, "JpegTableAgreement");
        bind_reflected<JpegPixelAgreement>(m, "JpegPixelAgreement");
        bind_reflected<RecompressionOutcome>(m, "RecompressionOutcome");
        bind_reflected<CompressionHistoryEvidence>(m, "CompressionHistoryEvidence");

        nb::class_<CompressionHistory>(m, "CompressionHistory")
            .def_ro("record", &CompressionHistory::record)
            .def_ro("configuration", &CompressionHistory::configuration)
            .def_prop_ro("evidence", &CompressionHistory::evidence, nb::rv_policy::reference_internal,
                         "The traces, and the JPEG tables, chroma evidence and recompression curves they rest on.")
            .def("to_dict", [](const CompressionHistory& self) { return to_python(self.to_json()); });

        m.def("compression_history",
              nb::overload_cast<const Frame&, const CompressionHistoryOptions&>(&compression_history), "frame"_a,
              "options"_a = CompressionHistoryOptions{}, nb::call_guard<nb::gil_scoped_release>());
        m.def("compression_history",
              nb::overload_cast<const DecodedImage&, const CompressionHistoryOptions&>(&compression_history),
              "image"_a, "options"_a = CompressionHistoryOptions{}, nb::call_guard<nb::gil_scoped_release>());
    }
}
