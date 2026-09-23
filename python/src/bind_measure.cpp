#include "bind_reflected.hpp"
#include "bindings.hpp"
#include "json_convert.hpp"

#include "lossylab/measure/measure.hpp"

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

        nb::class_<FrameMeasurement>(m, "FrameMeasurement")
            .def_ro("index", &FrameMeasurement::index)
            .def_ro("values", &FrameMeasurement::values)
            .def_ro("content_rect", &FrameMeasurement::content_rect)
            .def("value", &FrameMeasurement::value, "name"_a)
            .def("to_dict", [](const FrameMeasurement& self) { return to_python(self.to_json()); });

        nb::class_<MeasureResult>(m, "MeasureResult")
            .def_ro("frames", &MeasureResult::frames)
            .def_ro("pooled", &MeasureResult::pooled)
            .def_ro("record", &MeasureResult::record)
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

        bind_reflected_rw<RecompressionOptions>(m, "RecompressionOptions");

        nb::class_<RecompressionPoint>(m, "RecompressionPoint")
            .def_ro("quality_parameter", &RecompressionPoint::quality_parameter)
            .def_ro("error", &RecompressionPoint::error)
            .def_ro("bits_per_pixel", &RecompressionPoint::bits_per_pixel);

        nb::class_<RecompressionCurve>(m, "RecompressionCurve")
            .def_ro("points", &RecompressionCurve::points)
            .def_ro("estimated_prior_parameter", &RecompressionCurve::estimated_prior_parameter)
            .def_ro("confidence", &RecompressionCurve::confidence)
            .def_ro("record", &RecompressionCurve::record)
            .def("to_dict", [](const RecompressionCurve& self) { return to_python(self.to_json()); });

        m.def("recompression_curve", &recompression_curve, "frame"_a, "options"_a,
              nb::call_guard<nb::gil_scoped_release>());
    }
}
