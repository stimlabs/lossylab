#include "bind_reflected.hpp"
#include "bindings.hpp"
#include "json_convert.hpp"

#include "lossylab/pipeline/pipeline.hpp"

#include <nanobind/stl/string.h>
#include <nanobind/stl/string_view.h>
#include <nanobind/stl/vector.h>

namespace lossylab::pybind
{
    using namespace nb::literals;

    void bind_pipeline(nb::module_& m)
    {
        nb::class_<StageSpec>(m, "StageSpec")
            .def(nb::init<StageConfiguration, std::string>(), "configuration"_a, "label"_a = "")
            .def_rw("configuration", &StageSpec::configuration)
            .def_rw("label", &StageSpec::label)
            .def_prop_ro("kind", &StageSpec::kind)
            .def("to_dict", [](const StageSpec& self) { return to_python(self.to_json()); })
            .def_static("from_dict", [](nb::dict value) { return StageSpec::from_json(to_json(value)); });

        nb::class_<PipelineSpec>(m, "PipelineSpec")
            .def(nb::init<>())
            .def(nb::init<std::vector<StageSpec>>(), "stages"_a)
            .def(
                "add", [](PipelineSpec& self, StageConfiguration configuration, std::string label) -> PipelineSpec&
                { return self.add(std::move(configuration), std::move(label)); },
                "configuration"_a, "label"_a = "", nb::rv_policy::reference,
                "Appends a stage: the options of the operation it runs. Returns the spec, for chaining.")
            .def("stages", &PipelineSpec::stages)
            .def("__len__", &PipelineSpec::size)
            .def_prop_rw("source_sha256", &PipelineSpec::source_sha256, &PipelineSpec::set_source_sha256,
                         nb::arg("hash").none())
            .def("validate", nb::overload_cast<>(&PipelineSpec::validate, nb::const_))
            .def("spec_id", &PipelineSpec::spec_id)
            .def("to_dict", [](const PipelineSpec& self) { return to_python(self.to_json()); })
            .def_static("from_dict", [](nb::dict value) { return PipelineSpec::from_json(to_json(value)); })
            .def_static("parse", &PipelineSpec::parse, "text"_a)
            .def_static("from_record", &PipelineSpec::from_record, "record"_a)
            .def("__eq__", [](const PipelineSpec& self, const PipelineSpec& other) { return self == other; });

        nb::class_<PipelineResult>(m, "PipelineResult")
            .def_rw("frame", &PipelineResult::frame)
            .def_rw("record", &PipelineResult::record);

        nb::enum_<RecordDetail>(m, "RecordDetail")
            .value("Full", RecordDetail::Full)
            .value("Lean", RecordDetail::Lean);

        nb::class_<Pipeline>(m, "Pipeline")
            .def(nb::init<PipelineSpec>(), "spec"_a)
            .def("run", nb::overload_cast<const Source&, std::uint64_t, RecordDetail>(&Pipeline::run, nb::const_),
                 "source"_a, "seed"_a = 0, "record_detail"_a = RecordDetail::Full,
                 nb::call_guard<nb::gil_scoped_release>())
            .def("run", nb::overload_cast<const Frame&, std::uint64_t, RecordDetail>(&Pipeline::run, nb::const_),
                 "frame"_a, "seed"_a = 0, "record_detail"_a = RecordDetail::Full,
                 nb::call_guard<nb::gil_scoped_release>())
            .def("spec", &Pipeline::spec, nb::rv_policy::reference_internal);
    }
}
