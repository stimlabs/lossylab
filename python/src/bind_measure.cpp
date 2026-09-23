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

        nb::class_<MeasureOptions>(m, "MeasureOptions")
            .def(nb::init<>())
            .def_rw("strict", &MeasureOptions::strict);

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
    }
}
